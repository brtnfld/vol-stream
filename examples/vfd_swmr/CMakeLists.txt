# Built only when the HDF5 in use actually has VFD SWMR -- see
# VOL_STREAM_HAVE_VFD_SWMR in the top-level CMakeLists.txt. VFD SWMR is not in
# mainline HDF5 2.x, so on a stock build this directory is skipped entirely and
# nothing else is affected.
#
# No add_test(): this is a demonstration, not a gate. It needs two processes
# and a specific HDF5, and CI has no VFD SWMR leg. Run it by hand with
# run_poc.sh.

# Only the writer links the connector. The reader is deliberately plain native
# HDF5 -- see its own comment for why.
add_executable(vs_swmr_writer vs_swmr_writer.c)
target_link_libraries(vs_swmr_writer PRIVATE vol_stream)

add_executable(vs_swmr_reader vs_swmr_reader.c)
target_include_directories(vs_swmr_reader PRIVATE ${HDF5_INCLUDE_DIRS})
target_link_libraries(vs_swmr_reader PRIVATE ${HDF5_C_LIBRARIES})
if(MPI_C_FOUND)
  target_include_directories(vs_swmr_reader PRIVATE ${MPI_C_INCLUDE_DIRS})
  target_link_libraries(vs_swmr_reader PRIVATE ${MPI_C_LIBRARIES})
endif()

configure_file(run_poc.sh run_poc.sh COPYONLY)
