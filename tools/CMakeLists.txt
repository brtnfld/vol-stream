# M9: h5stream, the tool that understands steps.
#
# Links against vol_stream because `list` asks the connector for the logical
# step history (H5Fget_logical_steps) -- the one question only the connector
# can answer, since it alone knows which occurrence of a restarted logical id
# is authoritative. Everything else it reports comes from plain HDF5 calls,
# deliberately: the tool's job is to show what a plain HDF5 reader would find.
add_executable(h5stream h5stream.c)
target_link_libraries(h5stream PRIVATE vol_stream)
