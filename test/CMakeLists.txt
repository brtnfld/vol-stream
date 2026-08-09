add_executable(t_smoke t_smoke.c)
target_link_libraries(t_smoke PRIVATE vol_stream)

add_test(NAME smoke COMMAND t_smoke)
set_tests_properties(smoke PROPERTIES
  ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
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
