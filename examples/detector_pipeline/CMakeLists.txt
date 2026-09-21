# The P02.2-shaped detector pipeline: detector_writer plus pipeline_consumer
# (one binary, five modes -- discover/archive/viewer/analysis/hitfinder).
# The use case behind RFC-VOLSTREAM-2026-001 section A.2. See README.md.
add_executable(detector_writer detector_writer.c)
target_link_libraries(detector_writer PRIVATE vol_stream)

add_executable(pipeline_consumer pipeline_consumer.c)
target_link_libraries(pipeline_consumer PRIVATE vol_stream)
