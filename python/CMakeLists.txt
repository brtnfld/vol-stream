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
set(_volstream_py __init__.py _file.py torch.py)
foreach(py ${_volstream_py})
  configure_file(volstream/${py} ${CMAKE_CURRENT_BINARY_DIR}/volstream/${py} COPYONLY)
endforeach()

# Installed by `pip install .` (pyproject.toml builds only this component).
# libvol_stream goes inside the package and the extension finds it there; the
# library keeps RPATHs to the HDF5 and Mochi it was built against, since the
# point of a source build is to use exactly those.
if(SKBUILD)
  if(APPLE)
    set(_origin "@loader_path")
  else()
    set(_origin "$ORIGIN")
  endif()
  # The extension resolves exactly two libraries through its RPATH:
  # libvol_stream beside it, and libhdf5. List them explicitly rather than
  # using INSTALL_RPATH_USE_LINK_PATH, which also records the build directory
  # it linked libvol_stream from -- a temporary directory under pip, and not
  # recognised as the build tree on macOS, where /var is a symlink.
  set(_hdf5_rpath "")
  foreach(lib ${HDF5_C_LIBRARIES})
    set(_loc "")
    if(TARGET ${lib})
      get_target_property(_loc ${lib} IMPORTED_LOCATION)
      if(NOT _loc)
        get_target_property(_cfgs ${lib} IMPORTED_CONFIGURATIONS)
        foreach(c ${_cfgs})
          get_target_property(_loc ${lib} IMPORTED_LOCATION_${c})
          if(_loc)
            break()
          endif()
        endforeach()
      endif()
    elseif(IS_ABSOLUTE "${lib}")
      set(_loc "${lib}")
    endif()
    if(_loc)
      get_filename_component(_dir "${_loc}" DIRECTORY)
      list(APPEND _hdf5_rpath "${_dir}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _hdf5_rpath)
  set_target_properties(_volstream PROPERTIES INSTALL_RPATH "${_origin};${_hdf5_rpath}"
                                              INSTALL_RPATH_USE_LINK_PATH FALSE)
  set_target_properties(vol_stream PROPERTIES INSTALL_RPATH "${VOL_STREAM_DEP_LIBDIRS}"
                                              INSTALL_RPATH_USE_LINK_PATH TRUE)
  install(TARGETS _volstream vol_stream LIBRARY DESTINATION volstream COMPONENT python)
  list(TRANSFORM _volstream_py PREPEND volstream/)
  install(FILES ${_volstream_py} DESTINATION volstream COMPONENT python)
endif()

if(VOL_STREAM_BUILD_TESTS)
  set(_py_env "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR};HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>")

  add_executable(py_stream_fixture tests/stream_fixture.c)
  target_link_libraries(py_stream_fixture PRIVATE vol_stream)

  # P1 exit gate: import, open and close with no transport.
  add_test(NAME python_open
    COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_open.py
            $<TARGET_FILE:py_stream_fixture>)
  set_tests_properties(python_open PROPERTIES ENVIRONMENT "${_py_env}" TIMEOUT 60)

  # P2 exit gate: whole steps from a live C writer. The scripts default
  # VOL_STREAM_NA to na+sm themselves rather than having it forced here, so
  # CI's ofi+tcp pass can run them over ofi+tcp by exporting it.
  if(VOL_STREAM_HAVE_MERCURY)
    add_executable(py_stream_writer tests/stream_writer.c)
    target_link_libraries(py_stream_writer PRIVATE vol_stream)
    # P2 and P4: whole steps, iteration, get()-only, and the torch dataset
    # (skipped when torch is not installed).
    foreach(mode column narrowing iterate getonly torch eos drop ack noack)
      add_test(NAME python_stream_${mode}
        COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_stream.py
                $<TARGET_FILE:py_stream_writer> ${mode})
      set_tests_properties(python_stream_${mode} PROPERTIES
        ENVIRONMENT "${_py_env}"
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
        ENVIRONMENT "${_py_env}"
        TIMEOUT 120
      )
    endforeach()
  endif()
endif()
