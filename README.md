# vol-stream

An HDF5 VOL connector for **streaming** — step-based, reader-driven data movement
between a running simulation and whatever consumes its output, without going
through a filesystem.

> **Status: M0.** This is the pass-through skeleton. Every callback forwards to the
> underlying connector unchanged, so behaviour is identical to native HDF5. The
> step API is registered and discoverable but is a no-op. Nothing here is a
> supported HDF Group product, and the design is not yet reviewed.

## Why

HDF5 has five partial answers to streaming and no complete one: classic SWMR,
the VFD SWMR prototype, the mirror VFD, the onion VFD, and the Async VOL. Each
is useful and each stops short in a different place. The gap is not transport —
the mirror VFD already ships bytes over TCP — it is that **HDF5 has no step**.

The goal is not to match [ADIOS2](https://adios2.readthedocs.io/) but to go past
it, and the central bet is that streaming should be **reader-driven**: consumers
declare what they want, and the writer marshals only that. ADIOS2 pushes whole
steps and lets readers discard what they did not need. Doing better requires
owning the wire protocol, which is why this is not built on ADIOS2's SST.

Two documents carry the reasoning:

| | |
|---|---|
| [`docs/design-plan.md`](docs/design-plan.md) | Why the VOL and not a VFD, where ADIOS2 is beatable, what to borrow |
| [`docs/dev-plan.md`](docs/dev-plan.md) | The nine milestones, resolved design decisions, CI matrix |

## Design in one paragraph

Streaming belongs at the **VOL** layer, not the VFD layer: below the VOL, the
metadata cache and chunk index perform read-modify-write on offsets already
written, and a stream cannot seek backwards. HDF5 removed its 1.8-era
`H5FD_STREAM` driver for exactly this reason. At the VOL layer a write is still
*described* — dataset, datatype, dataspace, selection — so it serializes
forward-only and a reader can select into it arbitrarily.

The connector requires **no changes to HDF5**. `H5VLregister_opt_operation()`
lets it define its own operations, and `H5VLfile_optional_op()` is public, so the
step API ships in this repo's own header.

## Building

Requires HDF5 1.14+ (developed against `develop`, `H5VL_VERSION` 3) and, for a
parallel HDF5, an MPI implementation.

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/hdf5-install
cmake --build build
ctest --test-dir build --output-on-failure
```

Against an **uninstalled HDF5 build tree** — normal when working on the library
itself, and something `find_package` cannot locate — supply both variables to
skip discovery:

```bash
cmake -S . -B build \
  -DHDF5_C_LIBRARIES=/path/hdf5/build/bin/libhdf5.so \
  -DHDF5_INCLUDE_DIRS="/path/hdf5/src;/path/hdf5/build/src;/path/hdf5/src/H5FDsubfiling"
```

The third include directory is needed because an in-tree `hdf5.h` includes
`H5FDsubfiling.h` from its own subdirectory; an install flattens that.

## Using it

As a plugin, with no application changes:

```bash
export HDF5_PLUGIN_PATH=/path/to/vol-stream/build
export HDF5_VOL_CONNECTOR="vol-stream"
./your_hdf5_application
```

Or explicitly, from an application that links the connector:

```c
#include "H5VLstream.h"

hid_t vol_id = H5VL_stream_register();
hid_t fapl   = H5Pcreate(H5P_FILE_ACCESS);
H5Pset_vol(fapl, vol_id, NULL);   /* NULL info defaults the under-VOL to native */

hid_t fid = H5Fcreate("out.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl);

const uint64_t step = 42;
H5Fbegin_step(fid, 1, &step);
/* ... H5Dwrite / H5Dwrite_multi as usual ... */
H5Fend_step(fid);
```

### The step API

Declared in [`src/H5VLstream.h`](src/H5VLstream.h). `H5F`-namespaced because a
step is a file-scoped transaction, following the precedent set by the HDF5 Async
VOL, which also ships `H5F*` calls from an out-of-tree connector.

| Call | Purpose |
|---|---|
| `H5Fbegin_step(fid, n, ids)` | Open a step, carrying zero or more *logical* step ids |
| `H5Fend_step(fid)` | Commit it atomically |
| `H5Fstep_status(fid, &st)` | Query step state |
| `H5Fsubscribe(fid, n, paths, spaces, plists)` | Reader declares interest |

The logical ids are deliberately separate from the connector's monotone physical
step counter. Restarting from a checkpoint replays ids already seen — openPMD hit
this against ADIOS2 in production, where resuming at iteration 500 after writing
through 750 yields `0, 50 … 750, 500, 550` — and one monotone counter cannot
represent it. Carrying an explicit annotation fixes a bug ADIOS2 still has.

Because the operations are registered rather than compiled into HDF5, they are
discoverable at runtime:

```c
int op; uint64_t flags;
H5VLfind_opt_operation(H5VL_SUBCLS_FILE, H5VL_STREAM_OP_BEGIN_STEP, &op);
H5VLquery_optional(fid, H5VL_SUBCLS_FILE, op, &flags);
/* flags & H5VL_OPT_QUERY_COLLECTIVE -> must be called by every rank */
```

That reporting is what substitutes for a dedicated streaming capability flag,
which would need a library change.

## Plugin signatures

HDF5 can be built with `-DHDF5_REQUIRE_SIGNED_PLUGINS=ON`, and such a build
refuses to load an unsigned connector:

```
H5PL__read_and_validate_footer(): not a signed HDF5 plugin
```

This is easy to mistake for a broken plugin. See
[`docs/plugin-signing.md`](docs/plugin-signing.md) for how to sign a build and
set up a keystore.

## Testing

`ctest` runs a smoke test that checks the connector loads, is actually the one in
use, round-trips data, and has its step operations registered and queryable.

The real **M0 exit gate** is HDF5's own API test suite, run natively and through
the connector, requiring the two to be *indistinguishable* — not merely that the
connector run passes, since a pass quietly becoming a skip is the regression this
is meant to catch. It needs an HDF5 built with `-DHDF5_TEST_API=ON`:

```bash
./test/run_api_suite.sh --api-bin /path/hdf5-build/bin --plugin-dir ./build
```

## Roadmap

| | Milestone | Status |
|---|---|---|
| M0 | Skeleton, CI, regression net | **in progress** |
| M1 | Step API surface, as no-ops | partially landed |
| M2 | flatcc manifest, capture, replay invariant | |
| M3 | Decoupled reader | |
| M4 | Mercury transport, Margo progress, deferred I/O | |
| M5 | SSG rendezvous and late joiners | |
| M6 | Parallel writer and real M×N | |
| M7 | Queue policy and BAKE spill | |
| M8 | Subscription protocol | |
| M9 | Tools, bindings, and the long tail | |

Only four things are written here rather than borrowed: step semantics, queue
policy, the subscription protocol, and the HDF5-to-step mapping. Everything else
comes from a library that does that one job better — HDF5's own
`H5Tencode`/`H5Sencode2`/`H5Pencode2` for the manifest contents, flatcc for its
framing, Mercury and Margo for transport and progress, SSG for membership. See
[`docs/dev-plan.md`](docs/dev-plan.md).

## Provenance and license

Derived from the HDF5 pass-through VOL connector (`src/H5VLpassthru.c`), which is
the documented starting point for a new connector. Same license as HDF5 — see
[LICENSE](LICENSE).
