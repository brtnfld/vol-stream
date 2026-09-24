# The volstream Python package: a subscriber-only CPython extension over the
# connector's C API, plus a pure-Python layer that reassembles steps. Needs
# NumPy at run time only. See docs/python-plan.md.
#
# The package is assembled in ${CMAKE_CURRENT_BINARY_DIR}/volstream, so
# PYTHONPATH=<build>/python makes `import volstream` work from the build tree.

find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)

Python_add_library(_volstream MODULE WITH_SOABI _volstream.c)
target_link_libraries(_volstream PRIVATE vol_stream)
set_target_properties(_volstream PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/volstream
)
foreach(py __init__.py _file.py torch.py)
  configure_file(volstream/${py} ${CMAKE_CURRENT_BINARY_DIR}/volstream/${py} COPYONLY)
endforeach()

if(VOL_STREAM_BUILD_TESTS)
  set(_py_env "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR};HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>")

  add_executable(py_stream_fixture tests/stream_fixture.c)
  target_link_libraries(py_stream_fixture PRIVATE vol_stream)

  # P1 exit gate: import, open and close with no transport.
  add_test(NAME python_open
    COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_open.py
            $<TARGET_FILE:py_stream_fixture>)
  set_tests_properties(python_open PROPERTIES ENVIRONMENT "${_py_env}" TIMEOUT 60)

  # P2 exit gate: whole steps from a live C writer, over na+sm.
  if(VOL_STREAM_HAVE_MERCURY)
    add_executable(py_stream_writer tests/stream_writer.c)
    target_link_libraries(py_stream_writer PRIVATE vol_stream)
    # P2 and P4: whole steps, iteration, get()-only, and the torch dataset
    # (skipped when torch is not installed).
    foreach(mode column narrowing iterate getonly torch eos)
      add_test(NAME python_stream_${mode}
        COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_stream.py
                $<TARGET_FILE:py_stream_writer> ${mode})
      set_tests_properties(python_stream_${mode} PROPERTIES
        ENVIRONMENT "${_py_env};VOL_STREAM_NA=na+sm"
        TIMEOUT 120
        SKIP_RETURN_CODE 77
      )
    endforeach()

    # P3 exit gate: early exit without close(), Ctrl-C and other threads
    # during a wait, and fork().
    foreach(mode break interrupt fork)
      add_test(NAME python_lifecycle_${mode}
        COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lifecycle.py
                $<TARGET_FILE:py_stream_writer> ${mode})
      set_tests_properties(python_lifecycle_${mode} PROPERTIES
        ENVIRONMENT "${_py_env};VOL_STREAM_NA=na+sm"
        TIMEOUT 120
      )
    endforeach()
  endif()
endif()
