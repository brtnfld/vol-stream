foreach(t t_smoke t_step t_replay t_reader t_deferred t_vl_reject t_manifest_cache t_ref_path t_ref_roundtrip
           t_vl_roundtrip t_attr_convert t_payload_compress t_step_rewrite t_reregister t_memspace t_manypaths
           t_dset_resize)
  add_executable(${t} ${t}.c)
  target_link_libraries(${t} PRIVATE vol_stream)
endforeach()

add_test(NAME smoke COMMAND t_smoke)
add_test(NAME step COMMAND t_step)
add_test(NAME replay COMMAND t_replay)
add_test(NAME reader COMMAND t_reader)
add_test(NAME deferred COMMAND t_deferred)
add_test(NAME vl_reject COMMAND t_vl_reject)
add_test(NAME manifest_cache COMMAND t_manifest_cache)
add_test(NAME ref_path COMMAND t_ref_path)
add_test(NAME ref_roundtrip COMMAND t_ref_roundtrip)
add_test(NAME vl_roundtrip COMMAND t_vl_roundtrip)
add_test(NAME attr_convert COMMAND t_attr_convert)
add_test(NAME payload_compress COMMAND t_payload_compress)
add_test(NAME step_rewrite COMMAND t_step_rewrite)
add_test(NAME reregister COMMAND t_reregister)
# Guards the capture path against ignoring mem_space_id -- see t_memspace.c's
# own comment. Asserts bracketed and unbracketed writes agree, which is the
# same equivalence t_step.c's byte-identity gate makes, extended to the axis
# the suite was uniform in.
add_test(NAME memspace COMMAND t_memspace)
# Exercises the path -> index hash map (H5VL_stream_pathmap_t) past its
# initial capacity, with colliding long-common-prefix keys. Every other test
# uses a handful of paths, so the map never grows or collides in them.
add_test(NAME manypaths COMMAND t_manypaths)
# H5Dset_extent() on a live cross-step handle -- the ADIOS2-style growing
# time-series array. Guards against a real bug found by running this: the
# resize wasn't captured at all, so it landed directly on an EARLIER step's
# already-committed real object, silently rewriting history. Now refused;
# see H5VL_stream_dataset_specific()'s own comment and t_dset_resize.c.
add_test(NAME dset_resize COMMAND t_dset_resize)

# M4 exit gate: the two-process Mercury round trip. M5 exit gate: SSG
# rendezvous, late joiners, and a departing reader that does not stall the
# writer. M7 exit gate: Block/Discard/Spill under a deliberately slow
# reader. M8 exit gate (first increment): the writer marshals only what is
# subscribed. M8.5: per-subscriber precision -- the subscription's own DCPL
# actually re-filters what goes on the wire. M9: predicate pushdown -- the
# writer evaluates a subscriber's value test and sends only what matches, or
# nothing. All need the transport, so all are only built/run when
# VOL_STREAM_HAVE_MERCURY is on (see ../CMakeLists.txt's
# VOL_STREAM_ENABLE_MERCURY option).
if(VOL_STREAM_HAVE_MERCURY)
  foreach(t t_transport t_rendezvous t_queue_policy t_subscribe t_precision t_precision_dual
           t_chunk_zerocopy t_chunk_shape_split t_subvolume_nd t_strided_write t_predicate t_subvolume_strided)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE vol_stream)
  endforeach()
  add_test(NAME transport COMMAND t_transport)
  add_test(NAME rendezvous COMMAND t_rendezvous)
  add_test(NAME queue_policy COMMAND t_queue_policy)
  add_test(NAME subscribe COMMAND t_subscribe)
  add_test(NAME precision COMMAND t_precision)
  add_test(NAME chunk_zerocopy COMMAND t_chunk_zerocopy)
  # M8.5 follow-up: a subscriber's requested chunk shape is honored, not
  # forced to a single chunk spanning the whole run -- see t_chunk_shape_
  # split.c's own top comment.
  add_test(NAME chunk_shape_split COMMAND t_chunk_shape_split)
  add_test(NAME subvolume_nd COMMAND t_subvolume_nd)
  add_test(NAME strided_write COMMAND t_strided_write)
  add_test(NAME predicate COMMAND t_predicate)
  add_test(NAME subvolume_strided COMMAND t_subvolume_strided)
  set_tests_properties(transport rendezvous queue_policy subscribe precision chunk_zerocopy chunk_shape_split subvolume_nd strided_write predicate subvolume_strided PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
    TIMEOUT 60
  )

  # Benchmark, not a correctness gate in the usual sense: the growing
  # time-series array (t_dset_resize.c's supported re-create-each-step
  # pattern) streamed for real over na+sm, reporting commit latency,
  # end-to-end latency, and throughput. See its own top comment.
  add_executable(b_stream_grow b_stream_grow.c)
  target_link_libraries(b_stream_grow PRIVATE vol_stream)
  add_test(NAME stream_grow_benchmark COMMAND b_stream_grow)
  set_tests_properties(stream_grow_benchmark PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
    TIMEOUT 120
  )

  # The O(N) counterpart: same growing array, but H5Dset_extent() + a
  # tail-only write through one persistent handle (t_dset_resize.c's case
  # 3) instead of a full rewrite every step. See its own top comment.
  add_executable(b_stream_grow_tail b_stream_grow_tail.c)
  target_link_libraries(b_stream_grow_tail PRIVATE vol_stream)
  add_test(NAME stream_grow_tail_benchmark COMMAND b_stream_grow_tail)
  set_tests_properties(stream_grow_tail_benchmark PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
    TIMEOUT 120
  )

  # Writer-side cost against subscriber count (1/2/4/8), which is what the
  # asynchronous data push actually changes -- the two benchmarks above both
  # measure a single subscriber, the case where it matters least. See its
  # own top comment.
  #
  # Built but deliberately NOT registered as a test: running it surfaced a
  # real, pre-existing bug that has nothing to do with the benchmark itself
  # -- at 4 and 8 subscribers the writer intermittently issues one fewer
  # data push than it has (subscriber, step) pairs, and the subscriber that
  # loses one then blocks until its H5Fget_subscribed_data() timeout. See
  # docs/dev-plan.md ("a subscriber's push can go missing at fan-out").
  # Registering it would make the suite red on that open bug rather than on
  # anything this benchmark is measuring; run it by hand until it is fixed.
  add_executable(b_push_fanout b_push_fanout.c)
  target_link_libraries(b_push_fanout PRIVATE vol_stream)

  # Every test above registers the connector explicitly, which leaves the
  # process default VOL native -- and that is exactly what hid the connector's
  # own scratch files inheriting vol-stream as their VOL and recursing back
  # into a replay in progress. This runs one of them the other way, through
  # HDF5_VOL_CONNECTOR, and asserts on the artifacts. See the script's own
  # comment.
  add_test(NAME plugin_scratch
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_plugin_scratch_test.sh
            $<TARGET_FILE:t_precision> $<TARGET_FILE_DIR:vol_stream>)
  set_tests_properties(plugin_scratch PROPERTIES TIMEOUT 90)
  # t_precision_dual.c meets the M8/M8.5 exit gate's literal wording (two
  # simultaneous subscribers, different precisions, one end_step) with three
  # real OS processes -- see its own top comment for the two real bugs found
  # and fixed while building it. Not part of the default `ctest` run: a
  # separate, deeper, already-diagnosed residual (Mercury's progress ULT can
  # block in an unbounded libfabric connect() to an already-departed peer
  # during teardown, well outside this project to fix) makes it occasionally
  # (not always -- correctness never fails) run past a normal timeout.
  # DISABLED, not omitted, so it stays discoverable in `ctest -N`/listings
  # even though ctest itself will not run a DISABLED test even under an
  # explicit `-R` filter -- run the built binary directly on demand:
  # test/t_precision_dual (from the build dir).
  add_test(NAME precision_dual COMMAND t_precision_dual)
  set_tests_properties(precision_dual PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
    TIMEOUT 60
    DISABLED TRUE
  )
  # M9: h5stream `tail` follows a *live* writer, which is only possible over
  # the transport -- a writer's open file cannot be read by another process
  # at all. So unlike list/export it belongs here, with the other
  # transport-dependent tests.
  add_executable(t_h5stream_tail t_h5stream_tail.c)
  target_link_libraries(t_h5stream_tail PRIVATE vol_stream)
  add_test(NAME h5stream_tail COMMAND t_h5stream_tail $<TARGET_FILE:h5stream>)
  set_tests_properties(h5stream_tail PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
    TIMEOUT 90
  )

  # t_queue_policy.c gates its Spill scenario on this at compile time -- the
  # library's own VOL_STREAM_HAVE_BAKE compile definition is PRIVATE to the
  # vol_stream target, so a consumer target needs its own copy to see it.
  if(VOL_STREAM_HAVE_BAKE)
    target_compile_definitions(t_queue_policy PRIVATE VOL_STREAM_HAVE_BAKE)
  endif()
endif()

# M6 exit gate (first increment): collective step boundaries and a
# parallel writer/reader over MPI, coprime rank counts. Needs MPI (the
# same MPI_C_FOUND ../CMakeLists.txt already links vol_stream against when
# present) and mpiexec/mpirun.
if(MPI_C_FOUND AND MPIEXEC_EXECUTABLE)
  add_executable(t_parallel t_parallel.c)
  target_link_libraries(t_parallel PRIVATE vol_stream)
  add_test(NAME parallel
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_parallel_test.sh
            --mpirun     ${MPIEXEC_EXECUTABLE}
            --bin        $<TARGET_FILE:t_parallel>
            --plugin-dir $<TARGET_FILE_DIR:vol_stream>
  )
  set_tests_properties(parallel PROPERTIES TIMEOUT 120)

  # dev-plan.md's CI matrix asks for coprime rank shapes because that is where
  # M×N projection bugs surface, and 3->2 alone is a thin sample: with only
  # two readers, most remainder cases cannot arise at all. 7->3 is the next
  # shape the matrix names.
  #
  # Left at the default 4 elements/rank on purpose: 7*4 = 28 across 3 readers
  # does NOT divide, so the readers take 10/9/9 and neither decomposition
  # lines up with the other. That uneven case is the realistic one and used
  # to be impossible to run -- t_parallel refused it outright ("global_size
  # 28 not divisible by 3 ranks") until slab_of()/owner_of() replaced the
  # divide-and-hope arithmetic on both sides.
  add_test(NAME parallel_7x3
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_parallel_test.sh
            --mpirun       ${MPIEXEC_EXECUTABLE}
            --bin          $<TARGET_FILE:t_parallel>
            --plugin-dir   $<TARGET_FILE_DIR:vol_stream>
            --writer-ranks 7
            --reader-ranks 3
  )
  set_tests_properties(parallel_7x3 PROPERTIES TIMEOUT 180)

  # M6.5 concentrator topology, both aggregation shapes dev-plan.md claims
  # as verified (3->2, one 3-rank concentrator group; 7->3, two groups of
  # 3 plus a singleton) -- previously only ever run manually (see
  # reference_vol_stream_local_env.md-style local verification), not
  # CI-wired at all. run_parallel_test.sh's own --concentration check
  # (grep for "concentrator rank ... wrote ... on behalf of") is the gate
  # against this silently no-op'ing again.
  add_test(NAME parallel_concentration
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_parallel_test.sh
            --mpirun       ${MPIEXEC_EXECUTABLE}
            --bin          $<TARGET_FILE:t_parallel>
            --plugin-dir   $<TARGET_FILE_DIR:vol_stream>
            --concentration 2
  )
  set_tests_properties(parallel_concentration PROPERTIES TIMEOUT 120)

  add_test(NAME parallel_concentration_7x3
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_parallel_test.sh
            --mpirun       ${MPIEXEC_EXECUTABLE}
            --bin          $<TARGET_FILE:t_parallel>
            --plugin-dir   $<TARGET_FILE_DIR:vol_stream>
            --writer-ranks 7
            --reader-ranks 3
            --concentration 3
  )
  set_tests_properties(parallel_concentration_7x3 PROPERTIES TIMEOUT 180)

  # A parallel writer with a queue policy set -- the combination end_step()
  # used to ignore outright, dispatching to the parallel replay without ever
  # consulting the policy, so M7's backpressure was serial-only.
  #
  # Making it work required reducing the whole decision across the
  # communicator (H5VL__stream_queue_lag_view()/_agree_and()), and the failure
  # mode of getting that wrong is a HANG, not wrong data: one rank skipping an
  # MPI_Allreduce its peers are blocked in. So the assertion is that the run
  # completes at all and stays byte-exact -- a divergence surfaces as this
  # test's own timeout, which is why the timeout is deliberately tight.
  #
  # Scope, stated plainly: this covers the no-pressure path only. The transport
  # comes up (the .vsgroup sidecar proves it) so the policy region and both of
  # its collectives execute on every rank every step, which is what pins the
  # lockstep property. But with no reader attached, no rank has a tracked
  # reader, so the ranks cannot disagree about lag here even if the reduction
  # were removed. Covering the case that actually applies backpressure needs a
  # lagging reader acking to a single writer rank -- the configuration where
  # per-rank views genuinely diverge -- and is not built yet.
  if(VOL_STREAM_HAVE_MERCURY)
    # The case parallel_queue_policy below cannot reach: a real reader, so the
    # lag view is genuinely asymmetric across ranks (a reader acks to exactly
    # one of them). Without the reduction the un-acked ranks race ahead and the
    # run HANGS rather than answering wrongly, which is why the timeout is
    # tight and the writer asserts its own per-rank stall.
    add_executable(t_parallel_lag t_parallel_lag.c)
    target_link_libraries(t_parallel_lag PRIVATE vol_stream)
    add_test(NAME parallel_lag
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_parallel_lag_test.sh
              --mpirun       ${MPIEXEC_EXECUTABLE}
              --bin          $<TARGET_FILE:t_parallel_lag>
              --plugin-dir   $<TARGET_FILE_DIR:vol_stream>
              --writer-ranks 3
    )
    set_tests_properties(parallel_lag PROPERTIES TIMEOUT 90)

    add_test(NAME parallel_queue_policy
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_parallel_test.sh
              --mpirun       ${MPIEXEC_EXECUTABLE}
              --bin          $<TARGET_FILE:t_parallel>
              --plugin-dir   $<TARGET_FILE_DIR:vol_stream>
              --writer-ranks 3
              --reader-ranks 2
              --queue-policy block
    )
    set_tests_properties(parallel_queue_policy PROPERTIES
      ENVIRONMENT "VOL_STREAM_NA=na+sm"
      TIMEOUT 120
    )
  endif()
endif()

# M9: h5stream's `list`, checked by running the tool and parsing what a user
# would actually see. Registered here rather than in tools/ so it sits with
# the rest of the suite; it takes the tool's path as an argument because the
# tool is a sibling target, not something on PATH.
add_executable(t_h5stream_list t_h5stream_list.c)
target_link_libraries(t_h5stream_list PRIVATE vol_stream)
add_test(NAME h5stream_list COMMAND t_h5stream_list $<TARGET_FILE:h5stream>)

add_executable(t_h5stream_export t_h5stream_export.c)
target_link_libraries(t_h5stream_export PRIVATE vol_stream)
add_test(NAME h5stream_export COMMAND t_h5stream_export $<TARGET_FILE:h5stream>)

# M9's last item: `h5stream history` turns each step into an addressable
# onion-VFD revision. Reads back with plain HDF5 and an onion fapl, no
# connector, since being readable without this project is the point.
add_executable(t_onion_history t_onion_history.c)
target_link_libraries(t_onion_history PRIVATE vol_stream)
add_test(NAME onion_history COMMAND t_onion_history $<TARGET_FILE:h5stream>)

set_tests_properties(h5stream_list h5stream_export onion_history PROPERTIES
  ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
  TIMEOUT 60
)

# VOL_STREAM_H5DIFF points t_replay (the M2 replay-invariant gate) at an
# h5diff binary when it is not on PATH -- e.g. an uninstalled HDF5 build
# tree, which is the normal setup when developing against the library
# itself (see the HDF5-discovery note at the top of ../CMakeLists.txt).
set(VOL_STREAM_H5DIFF "" CACHE FILEPATH "h5diff binary used by the t_replay test (default: PATH lookup)")

set_tests_properties(smoke step replay reader deferred vl_reject manifest_cache ref_path PROPERTIES
  ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>;VOL_STREAM_H5DIFF=${VOL_STREAM_H5DIFF}"
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
