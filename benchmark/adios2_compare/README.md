# vol-stream vs. ADIOS2 SST: growing time-series array, streamed

A one-to-one comparison of `test/b_stream_grow.c`'s workload (an
unlimited-dimension array extended by a fixed chunk each step, re-created and
rewritten in full every step -- the working alternative now that resizing a
live cross-step handle is refused, see `docs/dev-plan.md`) run for real
against ADIOS2's SST engine, its streaming engine and the fair counterpart to
vol-stream's Mercury transport (not BP file staging).

Same workload on both sides: 50 steps, `/series` growing by 8192 elements
(32 KiB) per step to 409600 elements (1.6 MiB) at the final step, streamed
between a real writer and reader in separate OS processes. Not part of
vol-stream's own build or CI -- ADIOS2 is not a project dependency, this is a
one-off comparison, kept here for reproducibility.

## Results (measured 2026-08-15, this machine, `ofi`/`sockets` fabric --
no RDMA hardware present on either side of the comparison)

| Metric | vol-stream (`ofi+tcp`) | ADIOS2 SST |
|---|---|---|
| Total bytes streamed (50 steps) | 39.84 MiB | 39.84 MiB (identical workload) |
| Writer commit latency, mean | ~5.5-7 ms | ~0.2 ms |
| Writer commit latency, max | up to 87 ms (outlier observed) | ~0.5 ms |
| Start-to-receipt latency, mean | ~0.7-0.85 ms | ~0.6-2.7 ms (noisier run to run) |
| Aggregate throughput | ~145-148 MiB/s | ~2800-3500 MiB/s |

See `docs/dev-plan.md`'s benchmark section for the full write-up and what
this does and does not mean.

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
/home/brtnfld/packages/mpich/bin/mpicxx -std=c++14 -o adios2_bench adios2_bench.cpp \
  -I"$ADIOS2_PREFIX/include" \
  -L"$ADIOS2_PREFIX/lib64" -Wl,-rpath,"$ADIOS2_PREFIX/lib64" \
  -L"$LIBFABRIC_PREFIX/lib" -Wl,-rpath,"$LIBFABRIC_PREFIX/lib" \
  -ladios2_cxx11_mpi -ladios2_cxx11 -ladios2_core_mpi -ladios2_core

./run_adios2_bench.sh
python3 report.py
```

`ADIOS2_USE_MPI` must be `#define`d to `1` by the *application* before
including `adios2.h` -- the library is built with MPI support
(`ADIOS2_HAVE_MPI`), but the C++11 API's MPI-aware overloads (e.g.
`ADIOS(MPI_Comm)`) are only visible when the including translation unit
opts in itself. Easy to miss: the compiler error ("no matching function
for call") gives no hint that this is the cause.

## Design notes -- what makes this a fair comparison, and where it isn't

**Growing shape, expressed idiomatically per side.** vol-stream has no
supported way to resize a dataset across steps (see `test/t_dset_resize.c`);
its only working pattern is re-creating the object each step. ADIOS2 has a
real mechanism for this (`Variable::SetShape()`/`SetSelection()` before each
`Put()`, no redefinition needed) and it is used here rather than forcing
ADIOS2 through vol-stream's own workaround -- each side uses its own
idiomatic API for the *same data volume and growth pattern*, not a shared
code path.

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
