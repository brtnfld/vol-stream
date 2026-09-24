# The volstream Python package: a subscriber-only CPython extension over the
# connector's C API. See docs/python-plan.md.
#
# The package is assembled in ${CMAKE_CURRENT_BINARY_DIR}/volstream, so
# PYTHONPATH=<build>/python makes `import volstream` work from the build tree.

find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)

Python_add_library(_volstream MODULE WITH_SOABI _volstream.c)
target_link_libraries(_volstream PRIVATE vol_stream)
set_target_properties(_volstream PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/volstream
)
configure_file(volstream/__init__.py ${CMAKE_CURRENT_BINARY_DIR}/volstream/__init__.py COPYONLY)

if(VOL_STREAM_BUILD_TESTS)
  add_executable(py_stream_fixture tests/stream_fixture.c)
  target_link_libraries(py_stream_fixture PRIVATE vol_stream)

  # P1 exit gate: import, open and close with no transport.
  add_test(NAME python_open
    COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_open.py
            $<TARGET_FILE:py_stream_fixture>)
  set_tests_properties(python_open PROPERTIES
    ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR};HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
    TIMEOUT 60
  )
endif()
