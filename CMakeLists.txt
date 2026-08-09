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

add_library(vol_stream SHARED src/H5VLstream.c)

target_include_directories(vol_stream
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

if(HDF5_INCLUDE_DIRS)
  target_include_directories(vol_stream PUBLIC ${HDF5_INCLUDE_DIRS})
endif()

target_link_libraries(vol_stream PUBLIC ${HDF5_C_LIBRARIES})

if(MPI_C_FOUND)
  target_link_libraries(vol_stream PUBLIC MPI::MPI_C)
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
message(STATUS "vol-stream ${VOL_STREAM_VERSION}  (M0 - pass-through skeleton)")
message(STATUS "  HDF5:            ${HDF5_VERSION}")
message(STATUS "  logging:         ${VOL_STREAM_ENABLE_LOGGING}")
message(STATUS "  tests:           ${VOL_STREAM_BUILD_TESTS}")
message(STATUS "  h5sign:          ${H5SIGN_EXECUTABLE}")
message(STATUS "")
