# vol-stream vs. ADIOS2 SST: growing time-series array, streamed

A one-to-one comparison of `test/b_stream_grow.c`'s workload (an
unlimited-dimension array extended by a fixed chunk each step) run for real
against ADIOS2's SST engine, its streaming engine and the fair counterpart to
vol-stream's Mercury transport (not BP file staging). Two patterns, each
with its own vol-stream/ADIOS2 pair, since they move genuinely different
amounts of data, not just different API paths to the same bytes:

- **Full rewrite** (`adios2_bench.cpp` / `test/b_stream_grow.c`): every step
  re-sends the WHOLE cumulative array, 0..n-1. O(N^2) total bytes across the
  run. This is what `adios2_bench.cpp` actually does, on both sides equally
  -- an earlier version of this README claimed it used ADIOS2's tail-only
  idiom; it did not, and that was a real documentation error, now fixed here
  and in the pair below.
- **Tail-only** (`adios2_bench_tail.cpp` / `test/b_stream_grow_tail.c`):
  every step sends only the NEW tail slice. O(N) total bytes. ADIOS2's own
  idiomatic use of `SetSelection()` for a streaming append; vol-stream's
  counterpart needed `H5VL__stream_carry_forward_resized()` (see
  `docs/dev-plan.md` and `test/t_dset_resize.c`'s case 3) before it produced
  complete snapshots instead of a fill-value gap.

Same workload on both sides: 50 steps, `/series` growing by 8192 elements
(32 KiB) per step to 409600 elements (1.6 MiB) at the final step, streamed
between a real writer and reader in separate OS processes. Not part of
vol-stream's own build or CI -- ADIOS2 is not a project dependency, this is a
one-off comparison, kept here for reproducibility.

## Results (measured 2026-08-15, this machine, `ofi`/`sockets` fabric --
no RDMA hardware present on either side of the comparison)

### Full-rewrite pattern (O(N^2) total bytes)

| Metric | vol-stream, staging ON (default) | vol-stream, staging OFF | ADIOS2 SST |
|---|---|---|---|
| Total bytes streamed (50 steps) | 39.84 MiB | 39.84 MiB | 39.84 MiB (identical workload) |
| Writer commit latency, mean | ~5.7-7.3 ms | ~0.87-0.89 ms | ~0.2 ms |
| Writer commit latency, max | up to 87 ms (outlier observed) | ~1.9 ms | ~0.5 ms |
| Start-to-receipt latency, mean | ~0.7-0.85 ms | (not separately measured) | ~0.6-2.7 ms (noisier run to run) |
| Aggregate throughput | ~140-145 MiB/s | ~865-880 MiB/s | ~2800-3500 MiB/s |
| Total wall time, 50 steps | ~280 ms | ~46 ms | ~13 ms |

### Tail-only pattern (O(N) total bytes) -- the fairer, final comparison

| Metric | vol-stream, staging OFF | ADIOS2 SST |
|---|---|---|
| Total bytes streamed (50 steps) | 1.56 MiB | 1.56 MiB (identical workload) |
| Writer commit latency, mean | ~0.62-0.69 ms | ~0.021 ms |
| Total wall time, 50 steps | ~30-33 ms | ~2.05 ms |

**The gap does not close the way closing the O(N^2)-vs-O(N) gap inside
vol-stream itself might suggest -- it widens, relatively.** vol-stream's own
total wall time improved 1.4-1.5x moving to the tail-only pattern (46ms ->
~31.7ms), a real win measured and documented in `docs/dev-plan.md`. But
ADIOS2's total wall time improved far more (~13ms -> ~2.05ms, roughly 6x),
because ADIOS2's per-step cost scales down with less data far more than
vol-stream's does. The full-rewrite-vs-full-rewrite gap is ~3.6x; the
tail-only-vs-tail-only gap is ~15.5x. This is consistent with this
project's own profiling (`docs/dev-plan.md`'s `.payload`-staging section):
once payload-proportional costs are minimized, what's left on vol-stream's
side is a largely FIXED per-step cost (Mercury/Margo/Argobots RPC and
progress-engine overhead, real HDF5 metadata operations for a genuine
per-step object) that does not shrink with a smaller payload the way
ADIOS2's lighter-weight marshal-and-ship does. Closing *that* gap is a
different, harder problem than the O(N^2) one this session closed -- see
`docs/dev-plan.md`'s still-open options (async subscriber push, Margo
progress-engine tuning).

"Staging OFF" is `VOL_STREAM_STAGE_PAYLOAD=0` -- an existing, already-
documented, already-tested vol-stream knob (`H5VL__stream_stage_payload()`),
not a new feature. `perf record` on the writer found zlib `deflate()` (the
`.payload` staging dataset's compression, re-deflating this workload's
ever-larger cumulative payload from scratch every step) as the single
dominant cost with staging on; turning it off closes most of the gap to
ADIOS2, from roughly 20x down to roughly 3-4x. See `docs/dev-plan.md`'s
benchmark section for the full write-up, including why file-size-focused
measurement missed this CPU cost the first time around.

## Building ADIOS2 locally

Spack has `adios2` builtin. This machine's GCC 15 exposes a real bug in
ADIOS2 2.10.2's *bundled* `thirdparty/yaml-cpp` -- it's missing an explicit
`#include <cstdint>` that older libstdc++ used to pull in transitively,
so `uint16_t`/`uint32_t` go undeclared. Worked around with a forced
include rather than patching vendored source:

```
mkdir -p /some/scratch/adios2-spack-env
cp adios2-spack-env.yaml /some/scratch/adios2-spack-env/spack.yaml
spack -e /some/scratch/adios2-spack-env install
```

The env installs `adios2@2.10.2 +mpi +sst` with every unrelated
engine/compressor variant off, and shares this project's own
`install_tree` (`/home/brtnfld/.spack-opt`) so already-built `mpich`/
`libfabric` are reused rather than rebuilt.

## Building and running the benchmark

```
ENV=/some/scratch/adios2-spack-env
ADIOS2_PREFIX=$(spack -e "$ENV" location -i adios2)
LIBFABRIC_PREFIX=$(spack -e "$ENV" location -i libfabric)
for name in adios2_bench adios2_bench_tail; do
  /home/brtnfld/packages/mpich/bin/mpicxx -std=c++14 -o "$name" "$name.cpp" \
    -I"$ADIOS2_PREFIX/include" \
    -L"$ADIOS2_PREFIX/lib64" -Wl,-rpath,"$ADIOS2_PREFIX/lib64" \
    -L"$LIBFABRIC_PREFIX/lib" -Wl,-rpath,"$LIBFABRIC_PREFIX/lib" \
    -ladios2_cxx11_mpi -ladios2_cxx11 -ladios2_core_mpi -ladios2_core
done

./run_adios2_bench.sh && python3 report.py            # full-rewrite, O(N^2)
./run_adios2_bench_tail.sh && python3 report_tail.py   # tail-only, O(N)
```

The vol-stream side of each pair is `test/b_stream_grow.c` (full-rewrite)
and `test/b_stream_grow_tail.c` (tail-only) -- both built and run as part
of vol-stream's own test suite (`ctest -R stream_grow`), not this
directory; run them directly (`build/test/b_stream_grow[_tail]`) or via
`ctest` for the vol-stream-side numbers.

`ADIOS2_USE_MPI` must be `#define`d to `1` by the *application* before
including `adios2.h` -- the library is built with MPI support
(`ADIOS2_HAVE_MPI`), but the C++11 API's MPI-aware overloads (e.g.
`ADIOS(MPI_Comm)`) are only visible when the including translation unit
opts in itself. Easy to miss: the compiler error ("no matching function
for call") gives no hint that this is the cause.

## Design notes -- what makes this a fair comparison, and where it isn't

**Growing shape, expressed idiomatically per side -- and now in both of
its genuinely different forms.** At the time this comparison was first
built, vol-stream had no supported way to resize a dataset across steps
(see `test/t_dset_resize.c`'s history); the only working pattern was
re-creating the object each step (O(N^2) total bytes), so `adios2_bench.cpp`
used ADIOS2's own full-rewrite equivalent for a fair comparison at that
pattern -- not, as an earlier version of this README incorrectly claimed,
ADIOS2's tail-only idiom. vol-stream has since grown a real
`H5Dset_extent()` (`docs/dev-plan.md`) and, this same session, the
carry-forward mechanism that makes a tail-only write (O(N)) produce a
complete snapshot instead of a fill-value gap -- so `adios2_bench_tail.cpp`
now exists as the genuinely fair O(N)-vs-O(N) pair, using ADIOS2's real
`SetSelection()`-to-the-new-region idiom on one side and vol-stream's
matching tail-only pattern on the other. Each side still uses its own
idiomatic API, not a shared code path -- but now for both of the two
genuinely different amounts of data a growing array can be streamed as.

**Timestamps, not shared memory.** vol-stream's writer and reader are
`fork()` children of one process (a shared `mmap`), matching
`test/t_transport.c`'s existing pattern. SST's normal usage is two
*separate* `mpirun` jobs, not two ranks of one `MPI_COMM_WORLD`, so this
benchmark instead has each side append `(step, timestamp[, bytes])` lines to
a small per-step text file and computes latencies afterward. Valid because
`CLOCK_MONOTONIC` is one machine-wide clock, not a per-process one -- times
recorded by two unrelated processes on the same machine are directly
comparable without needing a shared memory segment or a common ancestor.

**What this does not claim.** No RDMA hardware exists on this machine, so
neither side is measuring an RDMA-capable network path -- both are on
plain sockets/TCP. A facility with real RDMA (what SST is designed for, and
what vol-stream's transport also supports via other `na` plugins) could
show a different gap. This is one workload (a growing array, single writer,
single reader, no MxN, no subscription narrowing) -- not a claim about every
pattern in `docs/design-plan.md`'s differentiator list, several of which
(subscription-based partial marshaling, predicate pushdown, per-subscriber
precision) have no ADIOS2 equivalent to compare against at all.
