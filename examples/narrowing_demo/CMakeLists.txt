# The "one write, narrowed per subscriber" demo: narrowing_writer plus
# narrow_subscriber (one binary, three modes -- float/gzip/predicate). See
# README.md.
add_executable(narrowing_writer narrowing_writer.c)
target_link_libraries(narrowing_writer PRIVATE vol_stream)

add_executable(narrow_subscriber narrow_subscriber.c)
target_link_libraries(narrow_subscriber PRIVATE vol_stream)
