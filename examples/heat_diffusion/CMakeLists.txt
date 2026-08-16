# A real use case: a 2D heat-diffusion solver (heat_writer) streaming its
# field live to a subscriber (heat_monitor) over the transport. See
# README.md. vol_stream re-exports HDF5/MPI/Mercury as PUBLIC link
# requirements, so linking it is all either target needs.
add_executable(heat_writer heat_writer.c)
target_link_libraries(heat_writer PRIVATE vol_stream)

add_executable(heat_monitor heat_monitor.c)
target_link_libraries(heat_monitor PRIVATE vol_stream)
