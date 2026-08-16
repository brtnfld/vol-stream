# A second real use case: Gray-Scott reaction-diffusion (rd_writer)
# streaming its V field live to a subscriber (rd_monitor). See README.md.
add_executable(rd_writer rd_writer.c)
target_link_libraries(rd_writer PRIVATE vol_stream)

add_executable(rd_monitor rd_monitor.c)
target_link_libraries(rd_monitor PRIVATE vol_stream)
