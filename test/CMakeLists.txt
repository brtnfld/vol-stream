foreach(t t_smoke t_step t_replay t_reader t_deferred)
  add_executable(${t} ${t}.c)
  target_link_libraries(${t} PRIVATE vol_stream)
endforeach()

add_test(NAME smoke COMMAND t_smoke)
add_test(NAME step COMMAND t_step)
add_test(NAME replay COMMAND t_replay)
add_test(NAME reader COMMAND t_reader)
add_test(NAME deferred COMMAND t_deferred)

# M4 exit gate: the two-process Mercury round trip. M5 exit gate: SSG
# rendezvous, late joiners, and a departing reader that does not stall the
# writer. Both need the transport, so both are only built/run when
# VOL_STREAM_HAVE_MERCURY is on (see ../CMakeLists.txt's
# VOL_STREAM_ENABLE_MERCURY option).
if(VOL_STREAM_HAVE_MERCURY)
  foreach(t t_transport t_rendezvous)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE vol_stream)
  endforeach()
  add_test(NAME transport COMMAND t_transport)
  add_test(NAME rendezvous COMMAND t_rendezvous)
  set_tests_properties(transport rendezvous PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
    TIMEOUT 60
  )
endif()

# VOL_STREAM_H5DIFF points t_replay (the M2 replay-invariant gate) at an
# h5diff binary when it is not on PATH -- e.g. an uninstalled HDF5 build
# tree, which is the normal setup when developing against the library
# itself (see the HDF5-discovery note at the top of ../CMakeLists.txt).
set(VOL_STREAM_H5DIFF "" CACHE FILEPATH "h5diff binary used by the t_replay test (default: PATH lookup)")

set_tests_properties(smoke step replay reader deferred PROPERTIES
  ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>;VOL_STREAM_H5DIFF=${VOL_STREAM_H5DIFF}"
)

# The M0 exit gate: HDF5's own test/API suite, run through the connector and
# compared against the same suite run natively. Opt-in, because it needs a
# separate HDF5 source tree configured with HDF5_TEST_API.
if(VOL_STREAM_HDF5_API_TESTS)
  add_test(NAME api_conformance
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_api_suite.sh
            --api-bin  ${VOL_STREAM_HDF5_API_TESTS}
            --plugin-dir $<TARGET_FILE_DIR:vol_stream>
  )
  set_tests_properties(api_conformance PROPERTIES TIMEOUT 3600)
endif()
