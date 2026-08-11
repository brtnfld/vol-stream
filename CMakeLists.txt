cmake_minimum_required(VERSION 3.18)
project(vol-stream C)

set(VOL_STREAM_VERSION 0.0.1)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# HDF5
#
# The connector is a plugin: it links libhdf5 and is dlopen'd by it. A build
# against the HDF5 the tests will run against is the only supported setup --
# H5VL_VERSION is compiled in, and a mismatch is the failure mode that broke
# the ADIOS2 connector when H5VL_VERSION moved from 0 to 2.
# ---------------------------------------------------------------------------
# Developing against an uninstalled HDF5 build tree is normal for anyone working
# on the library itself, and find_package cannot locate one. Pre-setting both
# variables skips discovery:
#
#   cmake -DHDF5_C_LIBRARIES=/path/hdf5/build/bin/libhdf5.so \
#         -DHDF5_INCLUDE_DIRS="/path/hdf5/src;/path/hdf5/build/src" ...
#
if(HDF5_C_LIBRARIES AND HDF5_INCLUDE_DIRS)
  message(STATUS "Using caller-supplied HDF5 (skipping find_package)")
  if(NOT HDF5_VERSION)
    set(HDF5_VERSION "caller-supplied")
  endif()
else()
  find_package(HDF5 1.14 REQUIRED COMPONENTS C)
endif()

# A parallel HDF5 pulls <mpi.h> in from H5public.h, so anything including
# hdf5.h needs the MPI include path -- including this connector. Not REQUIRED:
# a serial HDF5 neither needs nor wants it.
find_package(MPI COMPONENTS C)
if(MPI_C_FOUND)
  message(STATUS "MPI found: ${MPI_C_COMPILER}")
endif()

option(VOL_STREAM_ENABLE_LOGGING "Log every VOL callback to stdout" OFF)
option(VOL_STREAM_BUILD_TESTS "Build the test programs" ON)

# ---------------------------------------------------------------------------
# flatcc -- manifest framing (M2)
#
# The step manifest (src/vol_stream.fbs) is FlatBuffers, chosen so a later
# field (M8's predicate) is a compatible schema change rather than a wire
# break. flatcc has no exported CMake package config, so probe for a
# Spack-provided install first and fall back to vendoring via FetchContent.
#
# The vendored build needs two overrides confirmed against the actual
# v0.6.1 release, not assumed:
#   - CMAKE_POLICY_VERSION_MINIMUM=3.5: flatcc's own cmake_minimum_required
#     predates what modern CMake accepts.
#   - FLATCC_ALLOW_WERROR=OFF: flatccrt fails to build under GCC 15
#     (-Werror plus a -Wunterminated-string-initialization new in GCC 15,
#     in a bundled portable/grisu3_print.h shim flatcc doesn't control).
# ---------------------------------------------------------------------------
find_program(FLATCC_EXECUTABLE flatcc)
find_library(FLATCC_RUNTIME_LIBRARY flatccrt)
find_path(FLATCC_INCLUDE_DIR flatcc/flatcc_builder.h)

if(FLATCC_EXECUTABLE AND FLATCC_RUNTIME_LIBRARY AND FLATCC_INCLUDE_DIR)
  message(STATUS "flatcc: found (${FLATCC_EXECUTABLE})")
  set(VOL_STREAM_FLATCC_EXE ${FLATCC_EXECUTABLE})
  add_library(vol_stream_flatccrt UNKNOWN IMPORTED)
  set_target_properties(vol_stream_flatccrt PROPERTIES
    IMPORTED_LOCATION ${FLATCC_RUNTIME_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${FLATCC_INCLUDE_DIR})
  set(VOL_STREAM_FLATCC_RT_TARGET vol_stream_flatccrt)
else()
  message(STATUS "flatcc: not found, vendoring v0.6.1 via FetchContent")
  include(FetchContent)
  set(FLATCC_TEST OFF CACHE BOOL "" FORCE)
  set(FLATCC_CXX_TEST OFF CACHE BOOL "" FORCE)
  set(FLATCC_INSTALL OFF CACHE BOOL "" FORCE)
  set(FLATCC_ALLOW_WERROR OFF CACHE BOOL "" FORCE)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

  # flatcc_cli is a build-time code generator, invoked below to produce our
  # own headers -- it is never linked into vol_stream. A sanitizer build
  # (-fsanitize=... in CMAKE_C_FLAGS, e.g. the CI sanitizer matrix) would
  # otherwise instrument it too, and running the instrumented tool trips
  # real, pre-existing UB in flatcc's own code (misaligned loads in its
  # bundled cmetrohash, a null passed to a never-null codegen parameter)
  # that aborts the codegen step outright -- unrelated to whether our own
  # code has any issues. Shadow CMAKE_C_FLAGS (non-cache set(), so it only
  # affects this directory scope and everything FetchContent pulls in
  # here) with the -fsanitize= token stripped, then restore it immediately
  # after so vol_stream itself still gets the real flags.
  set(_vol_stream_saved_c_flags "${CMAKE_C_FLAGS}")
  string(REGEX REPLACE "-fsanitize=[A-Za-z,]+" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

  FetchContent_Declare(flatcc
    GIT_REPOSITORY https://github.com/dvidelabs/flatcc.git
    GIT_TAG        v0.6.1
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(flatcc)

  set(CMAKE_C_FLAGS "${_vol_stream_saved_c_flags}")
  unset(_vol_stream_saved_c_flags)
  unset(CMAKE_POLICY_VERSION_MINIMUM)
  set(VOL_STREAM_FLATCC_EXE $<TARGET_FILE:flatcc_cli>)
  set(VOL_STREAM_FLATCC_RT_TARGET flatccrt)
  # flatcc's own CMakeLists.txt uses directory-scoped include_directories()
  # rather than target_include_directories(), so linking against flatccrt
  # does not propagate its include path the way a normal target would --
  # add it explicitly.
  set(FLATCC_INCLUDE_DIR ${flatcc_SOURCE_DIR}/include)
endif()

# ---------------------------------------------------------------------------
# Mercury / Argobots / Margo / SSG -- transport, progress engine, and group
# membership (M4/M5)
#
# Optional. A build with none of these installed still produces a fully
# working connector: M0-M3's file-based capture/replay is untouched, and only
# the step_ready notification path and SSG-based rendezvous (src/tr_mercury.c)
# are skipped. Detection is via pkg-config, which all four ship -- borrow by
# default, per dev-plan.md, rather than a bespoke find module.
#
# Data still moves through the replicated /step/<n>/ file at these
# milestones, not over Mercury; Mercury/Margo carry the control-plane
# notification that a step committed, and SSG (M5) carries group membership
# and failure detection so a reader can join, leave, or die without a
# hand-rolled attach protocol or liveness tracking. See docs/dev-plan.md's
# M4 and M5 sections.
# ---------------------------------------------------------------------------
set(VOL_STREAM_ENABLE_MERCURY "AUTO" CACHE STRING
  "Build the Mercury/Margo/SSG transport (M4/M5): ON, OFF, or AUTO (default; enable if found)")
set_property(CACHE VOL_STREAM_ENABLE_MERCURY PROPERTY STRINGS AUTO ON OFF)

set(VOL_STREAM_HAVE_MERCURY FALSE)
if(NOT VOL_STREAM_ENABLE_MERCURY STREQUAL "OFF")
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(VOL_STREAM_MERCURY QUIET mercury)
    pkg_check_modules(VOL_STREAM_MARGO QUIET margo)
    pkg_check_modules(VOL_STREAM_ARGOBOTS QUIET argobots)
    pkg_check_modules(VOL_STREAM_SSG QUIET ssg)
  endif()

  if(VOL_STREAM_MERCURY_FOUND AND VOL_STREAM_MARGO_FOUND AND VOL_STREAM_ARGOBOTS_FOUND AND VOL_STREAM_SSG_FOUND)
    set(VOL_STREAM_HAVE_MERCURY TRUE)

    # pkg_check_modules()'s ordinary (non-static) result variables only
    # cover each module's own direct Libs -- not the Requires.private/
    # Libs.private closure (na, mercury_util, mchecksum, libfabric,
    # json-c, ...). That closure is needed here even though everything is
    # built shared: this system's linker does not resolve an executable's
    # undefined symbols by walking a directly-linked shared library's own
    # NEEDED entries at build time (only the dynamic loader does that,
    # later, at load time), so vol_stream's own link line has to carry the
    # full transitive closure. `pkg-config --static` computes exactly that;
    # FindPkgConfig has no equivalent option in this CMake version, so ask
    # the pkg-config binary directly -- all four modules in one call, so
    # pkg-config dedupes/orders the merged graph itself.
    execute_process(
      COMMAND ${PKG_CONFIG_EXECUTABLE} --static --libs mercury margo argobots ssg
      OUTPUT_VARIABLE VOL_STREAM_TRANSPORT_STATIC_LIBS
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE VOL_STREAM_TRANSPORT_PKGCONFIG_RC)
    execute_process(
      COMMAND ${PKG_CONFIG_EXECUTABLE} --static --cflags mercury margo argobots ssg
      OUTPUT_VARIABLE VOL_STREAM_TRANSPORT_STATIC_CFLAGS
      OUTPUT_STRIP_TRAILING_WHITESPACE)

    if(NOT VOL_STREAM_TRANSPORT_PKGCONFIG_RC EQUAL 0)
      message(FATAL_ERROR "pkg-config --static failed for mercury/margo/argobots/ssg")
    endif()

    separate_arguments(VOL_STREAM_TRANSPORT_STATIC_LIBS UNIX_COMMAND "${VOL_STREAM_TRANSPORT_STATIC_LIBS}")
    separate_arguments(VOL_STREAM_TRANSPORT_STATIC_CFLAGS UNIX_COMMAND "${VOL_STREAM_TRANSPORT_STATIC_CFLAGS}")
  elseif(VOL_STREAM_ENABLE_MERCURY STREQUAL "ON")
    message(FATAL_ERROR
      "VOL_STREAM_ENABLE_MERCURY=ON but mercury/margo/argobots/ssg were not all found via pkg-config")
  endif()
endif()

if(VOL_STREAM_HAVE_MERCURY)
  message(STATUS "Mercury transport: enabled (mercury ${VOL_STREAM_MERCURY_VERSION}, "
                  "margo ${VOL_STREAM_MARGO_VERSION}, argobots ${VOL_STREAM_ARGOBOTS_VERSION}, "
                  "ssg ${VOL_STREAM_SSG_VERSION})")
else()
  message(STATUS "Mercury transport: disabled (set VOL_STREAM_ENABLE_MERCURY=ON to require it)")
endif()

set(VOL_STREAM_FBS ${CMAKE_CURRENT_SOURCE_DIR}/src/vol_stream.fbs)
set(VOL_STREAM_FBS_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(VOL_STREAM_FBS_GEN_HDR ${VOL_STREAM_FBS_GEN_DIR}/vol_stream_builder.h)

add_custom_command(
  OUTPUT ${VOL_STREAM_FBS_GEN_HDR}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${VOL_STREAM_FBS_GEN_DIR}
  COMMAND ${VOL_STREAM_FLATCC_EXE} -a -o ${VOL_STREAM_FBS_GEN_DIR} ${VOL_STREAM_FBS}
  DEPENDS ${VOL_STREAM_FBS} $<$<TARGET_EXISTS:flatcc_cli>:flatcc_cli>
  COMMENT "Generating flatcc headers for vol_stream.fbs"
  VERBATIM)
add_custom_target(vol_stream_fbs_gen DEPENDS ${VOL_STREAM_FBS_GEN_HDR})

set(VOL_STREAM_SOURCES src/H5VLstream.c)
if(VOL_STREAM_HAVE_MERCURY)
  list(APPEND VOL_STREAM_SOURCES src/tr_mercury.c)
endif()

add_library(vol_stream SHARED ${VOL_STREAM_SOURCES})
add_dependencies(vol_stream vol_stream_fbs_gen)

target_include_directories(vol_stream
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${VOL_STREAM_FBS_GEN_DIR}
    ${FLATCC_INCLUDE_DIR}
)

if(HDF5_INCLUDE_DIRS)
  target_include_directories(vol_stream PUBLIC ${HDF5_INCLUDE_DIRS})
endif()

target_link_libraries(vol_stream PUBLIC ${HDF5_C_LIBRARIES})
target_link_libraries(vol_stream PRIVATE ${VOL_STREAM_FLATCC_RT_TARGET})

if(MPI_C_FOUND)
  target_link_libraries(vol_stream PUBLIC MPI::MPI_C)
endif()

if(VOL_STREAM_HAVE_MERCURY)
  # tr_mercury.c's reader-side wait/notify queue is guarded by a pthread
  # mutex/condvar, not an Argobots primitive -- see the note at the top of
  # that file for why.
  find_package(Threads REQUIRED)

  target_compile_definitions(vol_stream PRIVATE VOL_STREAM_HAVE_MERCURY)
  # PUBLIC: the full `pkg-config --static` closure (flags and libraries
  # alike) needs to reach consumers' own link lines too -- see the comment
  # above where these are computed.
  target_compile_options(vol_stream PUBLIC ${VOL_STREAM_TRANSPORT_STATIC_CFLAGS})
  target_link_libraries(vol_stream PUBLIC ${VOL_STREAM_TRANSPORT_STATIC_LIBS} Threads::Threads)
endif()

if(VOL_STREAM_ENABLE_LOGGING)
  target_compile_definitions(vol_stream PRIVATE ENABLE_STREAM_LOGGING)
endif()

# HDF5's plugin loader looks for an unversioned shared object on the search
# path, so do not decorate the name.
set_target_properties(vol_stream PROPERTIES
  OUTPUT_NAME vol_stream
  C_VISIBILITY_PRESET hidden
)

# ---------------------------------------------------------------------------
# Report the H5VL_VERSION being built against.
#
# This is pinned deliberately: the connector declares a struct version at
# compile time, and silently running against a library expecting a different
# one is the documented way connectors break.
# ---------------------------------------------------------------------------
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${HDF5_INCLUDE_DIRS})
if(MPI_C_FOUND)
  list(APPEND CMAKE_REQUIRED_INCLUDES ${MPI_C_INCLUDE_DIRS})
endif()
# Compile-only: this is a preprocessor fact, so there is nothing to link or run
# and no rpath to get wrong.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
check_c_source_compiles("
#include \"hdf5.h\"
#if H5VL_VERSION < 3
#error \"H5VL_VERSION < 3\"
#endif
int probe(void) { return H5VL_VERSION; }
" VOL_STREAM_H5VL_VERSION_OK)
unset(CMAKE_TRY_COMPILE_TARGET_TYPE)

if(NOT VOL_STREAM_H5VL_VERSION_OK)
  message(WARNING
    "Could not confirm H5VL_VERSION >= 3 against this HDF5. vol-stream is "
    "developed against 3; a connector built for a different VOL class struct "
    "version will fail to load -- the failure that broke the ADIOS2 connector "
    "when H5VL_VERSION moved. See docs/dev-plan.md, CI matrix.")
endif()


# ---------------------------------------------------------------------------
# Plugin signing
#
# HDF5 verifies plugin signatures before dlopen'ing them, and a build with
# enforcement on rejects an unsigned connector outright:
#
#   H5PL__read_and_validate_footer(): not a signed HDF5 plugin
#
# That makes signing a hard requirement for the HDF5_VOL_CONNECTOR path, which
# is how the M0 exit gate drives HDF5's test/API suite -- not an optional
# hardening step. Point VOL_STREAM_SIGN_KEY at a private key to sign as part of
# the build; readers then need HDF5_PLUGIN_KEYSTORE set to a directory holding
# the matching public key. See docs/plugin-signing.md.
# ---------------------------------------------------------------------------
set(VOL_STREAM_SIGN_KEY "" CACHE FILEPATH "RSA private key (PEM) used to sign the built plugin")
find_program(H5SIGN_EXECUTABLE h5sign HINTS ${HDF5_ROOT}/bin)

if(VOL_STREAM_SIGN_KEY)
  if(NOT H5SIGN_EXECUTABLE)
    message(FATAL_ERROR "VOL_STREAM_SIGN_KEY is set but h5sign was not found; set H5SIGN_EXECUTABLE.")
  endif()
  # -f so an incremental rebuild re-signs instead of failing on the old signature.
  add_custom_command(TARGET vol_stream POST_BUILD
    COMMAND ${H5SIGN_EXECUTABLE} -f -p $<TARGET_FILE:vol_stream> -k ${VOL_STREAM_SIGN_KEY}
    COMMENT "Signing $<TARGET_FILE_NAME:vol_stream> with ${VOL_STREAM_SIGN_KEY}"
    VERBATIM)
  message(STATUS "Plugin signing:  enabled")
else()
  message(STATUS "Plugin signing:  disabled (set VOL_STREAM_SIGN_KEY to enable)")
  message(STATUS "                 an unsigned plugin cannot be loaded via HDF5_VOL_CONNECTOR")
  message(STATUS "                 when the HDF5 in use enforces signatures")
endif()

if(VOL_STREAM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()

install(TARGETS vol_stream LIBRARY DESTINATION lib)
install(FILES src/H5VLstream.h DESTINATION include)

message(STATUS "")
message(STATUS "vol-stream ${VOL_STREAM_VERSION}  (M6 - parallel writer and real MxN, first increment)")
message(STATUS "  HDF5:            ${HDF5_VERSION}")
message(STATUS "  flatcc:          ${VOL_STREAM_FLATCC_EXE}")
message(STATUS "  mercury:         ${VOL_STREAM_HAVE_MERCURY}")
message(STATUS "  logging:         ${VOL_STREAM_ENABLE_LOGGING}")
message(STATUS "  tests:           ${VOL_STREAM_BUILD_TESTS}")
message(STATUS "  h5sign:          ${H5SIGN_EXECUTABLE}")
message(STATUS "")
