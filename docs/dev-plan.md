# vol-stream: development and implementation plan

A typeset version with diagrams is in [`dev-plan.pdf`](dev-plan.pdf). The
architectural reasoning is in [`design-plan.md`](design-plan.md).

Two rules govern this plan. **No changes to HDF5** — everything is reachable from
an out-of-tree connector. And **borrow by default** — we write the step
semantics, the queue policy and the subscription protocol, because those are the
differentiators; everything else already exists in a library that does that one
job better.

## The finding that closed three decisions

HDF5 can serialize its own data model. `H5Tencode` encodes any datatype,
`H5Sencode2` encodes a dataspace **including its selection**, and `H5Pencode2`
encodes a property list — so chunking and the whole filter pipeline travel with
the step.

The step manifest is therefore not a new format. It is a thin envelope around
blobs HDF5 produces and consumes, which makes the wire representation *exactly*
as expressive as the data model — compound types, enums, opaque types, odd byte
orders, arbitrary hyperslabs — with none of it hand-written. A reader
reconstructs types and selections with `H5Tdecode2` and `H5Sdecode` rather than
interpreting our description of them. The `H5Sencode2` selection encoding was
widened to 64-bit in the 1.12 series, so large selections are not a constraint.

## Verified plugin-side surface

Checked against HDF5 `develop`. These are the load-bearing APIs.

| API | Role |
|---|---|
| `H5VLregister_opt_operation` | Connector defines `begin_step`, `end_step`, `step_status`, `subscribe` at init. Public |
| `H5VLfile_optional_op` | How the shipped wrappers invoke them. Public, and `H5VLconnector.h` is installed |
| `H5VLquery_optional` | Applications and tools discover support via the connector's `opt_query` |
| `H5VL_request_class_t` | `wait`/`notify`/`cancel` — the deferred-Put queue and the step barrier |
| dataset read/write callbacks | Already take a `count` array, so `H5Dwrite_multi` maps onto a step batch |
| `H5Sselect_project_intersection` | M×N redistribution. Public, exercised by VDS |
| `H5Sselect_intersect_block` | Chunk-granularity routing |
| `H5VLpassthru.c` | A complete pass-through connector in-tree — the M0 starting point |
| `HDF5_VOL_CONNECTOR` | Points the whole `test/API` suite at the connector with no code changes |

### What the no-library-changes constraint costs

Two things assumed to need core work turn out not to:

- **An `H5ES` barrier for `end_step`.** Not needed. The connector creates its own
  request objects, so it already knows which belong to the open step and can wait
  over its own list.
- **Advertising what the connector cannot do.** `H5VLquery_optional` is public and
  `opt_query` can report per-operation support, covering the programmatic case.

Two are real:

- **Existing tools on a stream.** `h5dump` assumes random access and will fail
  unhelpfully. Without a capability flag there is no clean detection path, so the
  mitigation is documentation, a tested error-message matrix, and shipping
  `h5stream` early. If this becomes the top adoption complaint, that is the
  evidence for proposing the flag upstream — driven by reports, not speculation.
- **Automatic step boundaries.** `H5Pset_append_flush`'s callback is acted on by
  the native library, not an arbitrary connector, so the free auto-`end_step` is
  off the table. Applications call `end_step` explicitly, as they do with ADIOS2.

**Measured limit on the byte-identity promise (2026-08-14).** "Unbracketed
writes remain a pure pass-through, identical to native HDF5" holds only when
no transport is configured. With `VOL_STREAM_NA` set — either `na+sm` or
`ofi+tcp`, so this is not transport-specific — an unbracketed write produces
a file with the *same objects and the same data* but a different internal
allocation: 2995 bytes against native's 2816 in `t_step`'s own reference
case, the difference showing up as the superblock's end-of-file address.
`h5ls`/`h5dump` show no extra objects or attributes, so nothing is being
added to the logical file; the allocator simply takes a different path.

Found by running `t_step` under `VOL_STREAM_NA`, which nothing had done
before — every test that sets it is a transport test, and every test that
checks byte identity ran without one, so the two had never met. Worth stating
because the promise as written carries no qualification, and a reader would
reasonably expect it to hold for a transport-enabled writer too. In practice
a streaming writer is not the configuration anyone byte-compares, so this is
documented rather than fixed.

## The five decisions

### 1. Is a step file-wide, or does each group get its own cadence?

**Evidence.** openPMD hit this in production. Their ADIOS2 backend assumed one
step equals one logical iteration ascending, and it *broke on restart from
checkpoint*: resuming at iteration 500 after writing through 750 produces
`0, 50 … 750, 500, 550` — duplicate, non-monotone logical time on a monotone
physical counter. Their fix was not a second counter; it was an explicit
`snapshot` attribute annotating which logical iteration(s) a step carries.

**Decision.** A step is **file-scoped and physically monotone** — matching ADIOS2,
which keeps atomicity and the queue simple — but the manifest carries an
explicit, application-settable **set of logical step ids**, distinct from the
physical counter.

This resolves per-group cadence without a second mechanism: a group absent from
step *N* retains its value from its last appearance, and readers ask for "the
state of group G as of step N." A mesh written once per hundred field steps costs
one manifest entry, not a re-send. It also fixes the restart overlap ADIOS2 still
has.

### 2. How does the step index appear in the dataspace?

**Evidence.** openPMD standardized three encodings and ships all of them:
*groupBased*, one group per iteration at `/data/<n>/`, which their HDF5 backend
uses; *variableBased*, where a variable holds successive versions and no
per-iteration group exists, requiring ADIOS2; and *fileBased*, one file per
iteration. Nothing in the ecosystem models the step as an extra array dimension.

**This overturns an earlier recommendation.** Making the step an implicit
slowest-varying dimension conflates a transaction axis with a data axis, breaks
the moment a variable's per-step shape changes, and has no precedent anyone reads.

**Decision.** Two encodings, matching what exists. On the wire, steps are
successive versions of the same named object — openPMD's variable-based form,
which is simply what a stream is. On replay and in the file engine, steps land
group-based at `/step/<n>/`, the portable HDF5 encoding every existing tool can
read. The `[nsteps][…]` aggregate view remains available as an *optional VDS
overlay generated at replay*, never the primary model.

### 3. What is the subscription vocabulary?

**Evidence.** `H5Sencode2` encodes a dataspace with its selection, so a hyperslab
— including strided and irregular selections — is already a portable blob.
`H5Pencode2` encodes a filter pipeline with its parameters.

**Decision.** A subscription is a list of *(object path, `H5Sencode2` blob,
optional `H5Pencode2` blob)*. Variables, subvolumes and stride come from the
dataspace encoding; precision comes from the property-list encoding, so
per-subscriber precision reuses the existing filter pipeline instead of new
compression code.

Only the predicate is genuinely new, and it does not ship until M8. No reserved
slot is needed: because the records are **FlatBuffers tables**, adding a field
later is a compatible change old readers ignore.

### 4. Which parts of the data model are legal in v1?

**Evidence.** `H5Dvlen_get_buf_size` and `H5Treclaim` let a connector size and
release variable-length buffers, so it can deep-serialize an `hvl_t` itself. And
references are translatable: `H5Rget_obj_name`, `H5Rget_file_name` and
`H5Rget_attr_name` convert an opaque `H5R_ref_t` into names, which
`H5Rcreate_object` and `H5Rcreate_region` reconstruct on the far side — with the
region's selection carried by `H5Sencode2`.

**This overturns an earlier recommendation.** Excluding VL data cited VFD SWMR
hitting the same wall because HDF5 stores VL as metadata. That reasoning does not
transfer: VFD SWMR's problem is *concurrent metadata in a shared file*; a
connector serializing its own payload never touches the global heap.

**Decision.** v1 covers datasets, attributes, groups, committed datatypes, and
**variable-length data** via deep serialization. References are supported **by
name translation** when the target is inside the stream. The exclusion list
shrinks to references pointing outside the stream, which must be rejected at
`end_step` with a clear error rather than silently dangling.

### 5. What are the public calls named?

**Evidence.** HDF5's house style, visible in `test/API/H5_api_async_test.c`, is
to extend within the owning class namespace (`H5Dwrite_async`,
`H5Fcreate_async`) or add a new two-letter subsystem like `H5ES`. The Async VOL
sets the out-of-tree precedent by shipping `H5F`-namespaced calls.

**This fixes a collision.** An earlier draft used `H5SBegin_step`. `H5S` is the
dataspace namespace — actively misleading, and it would have shipped in a public
header.

**Decision.** `H5Fbegin_step()`, `H5Fend_step()`, `H5Fstep_status()`,
`H5Fsubscribe()`. Registered operation strings are namespaced independently as
`"vol-stream:begin_step"` so two connectors cannot collide in the registry.

## Implementation design

### Step manifest

```
// vol_stream.fbs -- compiled by flatcc. Adding a field is a compatible
// change, so there is no reserved slot to guess the size of.
namespace vs;

enum Kind    : ubyte { DsetCreate, DsetWrite, Attr, Group, Link }
enum Payload : ubyte { Raw, FilteredChunks }

table Entry {
  kind:Kind;
  path:string;
  type_enc:[ubyte];        // H5Tencode
  space_enc:[ubyte];       // H5Sencode2 -- carries the selection
  dcpl_enc:[ubyte];        // H5Pencode2 -- chunking + filter pipeline
  form:Payload;
  payload_off:ulong;
  payload_len:ulong;
  predicate:[ubyte];       // added in M8; absent in earlier writers
}

table Step {
  physical_step:ulong;     // monotone, connector-assigned
  logical_ids:[ulong];     // decision 1 -- restart-safe
  wall_time_ns:ulong;      // supplied by caller, never generated
  hdf5_version:uint;       // which library produced the blobs above
  entries:[Entry];
  payload_bytes:ulong;
}
root_type Step;
```

The `Step` table is serialized after all entries are known, so a step remains a
single forward-only append with no back-patching — the property that made the VOL
the right layer.

### Filtered-chunk passthrough

> **Status: not being built.** This section is retained as design rationale —
> the conflict rule below is live code — but `FilteredChunks` as a manifest
> payload form was decided against in M8.5.1; see that section for the
> evidence.

`H5Dread_chunk2`, `H5Dwrite_chunk`, `H5Dchunk_iter` and `H5Dget_chunk_info` are
public, so when a dataset is chunked and filtered and a write covers whole
chunks, the connector can move **already-compressed chunks** without
decompressing. That is the `FilteredChunks` payload form.

It cuts CPU on both ends, shrinks the wire payload for free, and a chunk is the
natural routing unit for subscription — paired with `H5Sselect_intersect_block`
the writer decides which chunks a subscriber needs without touching the data.
Partial-chunk writes degrade to `Raw`.

**This conflicts with per-subscriber precision (decision 3, M8.5) unless
resolved explicitly.** `FilteredChunks` bytes are already compressed under the
dataset's own native filter pipeline; a subscriber whose requested
`H5Pencode2` pipeline differs from that native one cannot be served those
bytes as-is. Rule: if any current subscriber's pipeline differs from the
dataset's native DCPL, the writer falls back from `FilteredChunks` to `Raw`
for that push — decompress, apply the subscriber's own pipeline, re-compress
— rather than sending mismatched or unfiltered bytes. `FilteredChunks` stays
zero-copy only for subscribers whose pipeline matches the write-time DCPL
exactly.

Per-subscriber precision shipped in M8.5, and the conflict rule above is now
**live code rather than a trivially-satisfied one** — though reached from the
opposite direction than this section assumed.

**M8.5.1 — the zero-copy fast path.** No new capture machinery or payload
form was needed to get the first real benefit. By the time a subscriber is
served, replay has *already* written a real, chunked, filtered dataset into
the underlying file — so when a subscriber's requested pipeline is the one
the data was written under, the compressed bytes it wants exist verbatim,
and `H5VL__stream_refilter_zero_copy()` reads them straight out
(`H5VL_NATIVE_DATASET_GET_CHUNK_INFO_BY_COORD` /
`H5VL_NATIVE_DATASET_CHUNK_READ`) instead of rebuilding an identical
throwaway `H5FD_CORE` dataset and re-running the same filters over the same
input to recompute the same answer.

That is exactly this section's rule, enforced by construction: matching
pipeline → serve stored filtered bytes; differing pipeline → fall back to
re-filtering from `Raw`. The equivalence test is `H5Pequal()` on the decoded
subscriber and write-time DCPLs, which compares chunking as well as filters,
so a subscriber asking for a different chunk shape correctly declines rather
than being silently served the dataset's own shape. Declining is always safe
— the slow path still produces a correct result.

Two constraints are deliberate, both matching this section's own "partial
writes degrade to `Raw`" rule: the push must cover the whole dataset from
element 0 (a partial overlap would need the chunk grid walked and per-chunk
intersection computed), and the extent must be exactly one chunk (the
receiving side reconstructs a single whole-extent chunk, so serving several
would need a wire-format change). Anything else declines to the slow path.

Both branches are covered by tests that distinguish *which* ran, not merely
that the data came out right: `VOL_STREAM_DEBUG_REFILTER` tags the fast
path's log line `(zero-copy)`, so `test/t_chunk_zerocopy.c` (matching
pipeline) fails if it silently falls through, and `test/t_precision.c`
(deliberately mismatched pipeline) continues to exercise the temporary-
dataset route.

**Step payload staging is compressed (2026-08-14).** Measuring before
building `FilteredChunks` turned up a bigger and much cheaper win. Every step
writes its raw bytes to `/step/<n>/.payload` and then replays the real
objects beside it, so the same data is stored twice — and for a filtered
dataset the two costs are nothing alike. On a 2000-int GZIP dataset the real
object allocated **48** bytes while `.payload` allocated the full **8000**:
the staging copy was 167x the size of the data it staged, and dominated the
file.

`.payload` is an opaque byte array this connector both writes and reads, so
deflating it needs no format change and no reader change — HDF5 inflates it
transparently. It now allocates **83 bytes instead of 8000** for that same
step, and the win is generic (every step, not only filtered datasets) rather
than confined to the case `FilteredChunks` would have addressed. Best-effort
throughout: no deflate available, or any property-list call failing, falls
back to the previous uncompressed layout rather than failing the commit.
`test/t_payload_compress.c` asserts it, because no behavioral test can —
compression is invisible to correctness checks, so a regression that dropped
it would leave the whole suite green while the file quietly grew back.

That measurement also changes the case for `FilteredChunks`: the duplication
it was meant to attack is now largely gone, and what remains of its value is
the CPU saved by not re-filtering at replay — which the M8.5.1 zero-copy path
already captures for the transport side.

**`FilteredChunks` as a manifest payload form: decided against.** Not
deferred — dropped, on evidence. It was motivated by two costs, and both
have since been addressed more cheaply:

- *Payload duplication.* The staging copy really was the dominant cost
  (167x the real data), but compressing `.payload` fixed that generically,
  for every step rather than only filtered datasets, in a few lines and with
  no format change.
- *Re-filtering CPU at replay.* M8.5.1's zero-copy fast path already avoids
  it wherever a subscriber's pipeline matches the dataset's, which is the
  case `FilteredChunks` was aimed at.

What remains of its value is a narrow slice — avoiding a re-filter for a
subscriber whose pipeline matches but whose write did not cover a whole
single chunk — bought at the price of a new payload form, a `filter_mask`
schema field, capture-side filtering, and a replay path that injects chunks
via `H5Dwrite_chunk()`. That is an M2-scale change for a case neither
measured nor reported. Revisit only if profiling shows re-filtering as a
real cost; the design above stands ready if so.

Still open (not dropped): chunk-grid-aware routing of a subscriber's N-D
selection. Subscription routing linearizes the selection's bounding box, so
a non-contiguous subscription receives a superset of what it asked for —
correct, but coarser than it could be.

### Connector state machine

| State | Legal operations | Notes |
|---|---|---|
| `OPEN` | create/open objects; `begin_step`; `subscribe` | Writes outside a step are an error, not an implicit step |
| `IN_STEP` | dataset and attribute writes; further creates; `end_step` | Entries accumulate; deferred writes queue on the connector's request list |
| `COMMITTING` | none from the application | Barrier over this step's requests, reference validation, manifest emit. Collective in parallel |
| `READING` | `begin_step` to advance; reads within the current step | Advancing past an unconsumed step is governed by queue policy |

### Registration and introspection

```c
herr_t vs_init(hid_t vipl_id) {
    H5VLregister_opt_operation(H5VL_SUBCLS_FILE, "vol-stream:begin_step",  &vs_op_begin);
    H5VLregister_opt_operation(H5VL_SUBCLS_FILE, "vol-stream:end_step",    &vs_op_end);
    H5VLregister_opt_operation(H5VL_SUBCLS_FILE, "vol-stream:step_status", &vs_op_stat);
    H5VLregister_opt_operation(H5VL_SUBCLS_FILE, "vol-stream:subscribe",   &vs_op_sub);
    return 0;
}

/* opt_query lets applications and tools discover this without a capability flag. */
herr_t vs_opt_query(void *obj, H5VL_subclass_t cls, int op, uint64_t *flags) {
    if (op == vs_op_begin || op == vs_op_end)
        *flags = H5VL_OPT_QUERY_SUPPORTED | H5VL_OPT_QUERY_COLLECTIVE
               | H5VL_OPT_QUERY_MODIFY_METADATA;
    ...
}
```

`H5VL_OPT_QUERY_COLLECTIVE` states that step boundaries must be called by every
rank, so the parallel contract is discoverable rather than only documented.

### Threading

**Argobots and Margo own the progress engine from M4**, not from the end. Margo
exists precisely to drive Mercury from user-level threads; the alternative was
writing a progress thread by hand for three milestones and then deleting it. The
connector keeps one lock over its own step and queue state.

v1 does not advertise `H5VL_CAP_FLAG_THREADSAFE`. Borrowing a good threading
library is not the same as promising the connector is safe under concurrent
application calls.

## Milestones

Sizes are relative — S is days, M is weeks, L is a quarter-ish for one developer.
Every gate is runnable.

### M0 — Skeleton, CI, and the regression net · S

Start from `H5VLpassthru.c`. Register as `vol-stream`, build as a shared plugin,
wire CI across every HDF5 version to support. No streaming logic — this exists to
make later milestones measurable.

**Exit gate.** `test/API` driven via `HDF5_VOL_CONNECTOR=vol-stream` behaves
identically to native. Not merely "passes": a pass quietly becoming a skip is the
regression this catches.

### M1 — Step API surface, as no-ops · S

Register the four namespaced operations, ship `H5VLstream.h` with the wrappers
over `H5VLfile_optional_op`, implement `opt_query` including the collective flag,
pin the `H5VL_VERSION` check.

**Exit gate.** An application brackets writes in `H5Fbegin_step`/`H5Fend_step`
and produces a byte-identical file to one written without them.
`H5VLquery_optional` reports each op correctly, including collectivity.

### M2 — Manifest, capture, and the replay invariant · M

The milestone carrying the weight an RFC would have. Define the flatcc schema and
fill it from `H5Tencode`/`H5Sencode2`/`H5Pencode2`; implement the logical-id set;
land steps group-based; implement VL deep-serialization and reference-by-name.

Then stand up the replay invariant: the same unmodified writer run through the
connector and through native HDF5 must produce indistinguishable files. Because
it compares against the library rather than a hand-written expectation, it catches
type-conversion, dataspace and ordering bugs nobody predicted.

**Exit gate.** `h5diff` clean across compound and enum types, byte-order
mismatch, committed datatypes, VL data, in-stream references, attributes, nested
groups, chunked and contiguous layouts, and partial hyperslab writes.
Out-of-stream references fail at `end_step` with a specific error. Filtered-chunk
passthrough round-trips, and partial-chunk writes demonstrably fall back to `Raw`.

### M3 — Decoupled reader · M

A separate reader process opens the stream, iterates steps, and reads with
hyperslabs unrelated to how the writer decomposed its writes. Single-process both
sides. Implement "state of group G as of step N" and the optional VDS overlay.

**Exit gate.** Selections straddling multiple writer calls return correct data. A
group written every hundredth step resolves correctly at every intermediate step.
A restart-overlap sequence is read back in correct logical order.

### M4 — Mercury transport, Margo progress, deferred I/O · M

Real transport arrives here rather than at the end. `tr_mercury` with Margo
driving progress on Argobots threads, and `H5VL_request_class_t` implemented so
writes queue and resolve at `end_step`.

**Exit gate.** `H5_api_async_test.c` passes. Writer and reader in separate
processes over Mercury on both `na+sm` and `ofi+tcp`, replay invariant intact
with deferral on.

### M5 — Rendezvous and late joiners · S

SSG for group membership — what SST's `RegistrationMethod`,
`RendezvousReaderCount` and late-joiner discovery amount to.

**Exit gate.** A writer starts with no readers and proceeds; a reader attaches at
step 500 and gets a coherent view; a reader leaving mid-stream does not stall the
writer. All without a shared filesystem.

### M6 — Parallel writer and real M×N · L

Collective `begin_step`/`end_step` over the writer communicator, per-rank
selections aggregated into the manifest, reader requests resolved with
`H5Sselect_project_intersection`. Aggregation topology borrowed from the
Subfiling VFD's I/O concentrators.

**Exit gate.** Byte-exact data with coprime rank counts — 7 writers to 3 readers,
64 to 5. Replay invariant holds in parallel.

M6's first increment meets this gate on the uniform-topology case only — every
rank creates the same objects, varying just which hyperslab each rank covers,
which is already legal under parallel HDF5's own collective-create rule and
needs no manifest aggregation across ranks. [M6.5](#m65--heterogeneous-mn-and-concentrator-topology--m)
carries the general case forward.

### M6.5 — Heterogeneous M×N and concentrator topology · M

The two things M6 named but didn't need for its first increment: ranks that
create *different* objects (rank 0 writes a dataset rank 1 never touches) and
the Subfiling-style I/O-concentrator aggregation topology. Heterogeneous
object sets break the "every rank calls the same creates" shortcut, so pending
entries must be exchanged across ranks — `MPI_Allgatherv` of a per-rank flatcc
mini-manifest (metadata only; payload bytes stay local), merged deterministically
(create-kind entries deduped by `(kind, path)`; `DsetWrite` never deduped;
`Attr` first-seen-wins). Reader-side requests spanning a concentrator's
aggregated region are resolved with `H5Sselect_project_intersection`, the
public API M6 traced but left unused.

**Exit gate.** Coprime rank counts (7→3, 64→5) with a heterogeneous per-rank
object set — not every rank creating the same objects — still byte-exact and
replay-invariant, routed through at least one concentrator that aggregates
more than one writer rank.

Cross-rank manifest aggregation and heterogeneous per-rank object sets shipped
and are verified byte-exact at this gate's own 7→3 and 64→5 rank counts, real
per-rank-private datasets included — see `H5VL__stream_replay_step_parallel()`
in `src/H5VLstream.c`.

**Update:** the Subfiling-style I/O-concentrator topology has also landed,
opt-in via `VOL_STREAM_CONCENTRATION` (unset/1 = every rank still does its own
raw-data I/O directly, unchanged from the paragraph above). Writer ranks are
partitioned into contiguous groups of that size; each group's first rank is
its concentrator, and every other member ships its `DsetWrite` entries to it
over MPI point-to-point instead of writing directly (type/dataspace carried
via `H5Tencode`/`H5Sencode2`, the same idiom the manifest replay already
uses) — see `H5VL__stream_replay_concentrated_writes()`. Verified byte-exact
with real aggregation at 3→2 (one 3-rank concentrator group) and 7→3 (two
concentrator groups of 3, one singleton), via
`run_parallel_test.sh --concentration N`, which also asserts the concentrator
actually logged writing on another rank's behalf rather than silently
no-op'ing. Still open: `H5Sselect_project_intersection`-based reader
resolution — this increment only changes *which rank* issues each
`H5VLdataset_write()`, not what a reader does, so a concentrator's aggregated
region is not yet combined into fewer/larger writes or specially resolved on
read.

### M7 — Queue policy and BAKE spill · M

`Block`, `Discard` and `Spill`, plus reserve slots and latest-step-only for
monitoring readers. BAKE moves bytes to node-local storage; the policy deciding
*when* is ours.

**Exit gate.** Under a deliberately slow reader: `Block` stalls the writer and
loses nothing, `Discard` drops only whole steps, `Spill` does neither and the
lagging reader catches up from local storage.

### M8 — Subscription protocol · L

The differentiator. Readers declare interest as *(path, `H5Sencode2`,
`H5Pencode2`)* triples; the writer marshals only what is subscribed, routing at
chunk granularity. Mercury's RPC direction, in place since M4, is the
back-channel.

**Exit gate.** Two subscribers on one stream at different precisions from a
single `end_step`. Measured wire bytes scale with subscribed volume, not total
step volume — the number that proves the thesis.

M8's first increment proves the core mechanism — a subscribe RPC and a real
data-push RPC (Mercury's first carrying payload bytes, not just control-plane
scalars) — at whole-object granularity: an unsubscribed sibling object in the
same step is never sent. [M8.5](#m85--chunk-level-subscription-routing--m)
carries the exit gate's literal chunk-level/multi-precision numbers forward.

### M8.5 — Chunk-level subscription routing · M

The two things M8 named but its first increment didn't need: routing a
subscription's *(path, `H5Sencode2` selection)* at chunk granularity via
`H5Sselect_intersect_block` against the `FilteredChunks` payload form (M2), and
per-subscriber precision — reusing a subscription's `H5Pencode2` filter
pipeline to re-filter one dataset's bytes differently per subscriber, per
dev-plan.md decision #3. Both need a subscription to carry (and the writer to
actually consult) the dataspace selection and property list M8's first
increment already accepts and validates but never acts on.

**Exit gate.** Two subscribers on one stream at different precisions from a
single `end_step`, wire bytes measured (not just asserted less-than) to scale
with subscribed volume — dev-plan.md's M8 exit gate, in full.

**Correctness fix 2026-08-14 — N-D selections were silently truncated.**
Routing is expressed in flat element indices, and the bounds helper used
`H5Sget_select_bounds()`'s *dimension-0* low/high directly as element
indices. For a 1-D dataspace that is the same thing, and every subscription
test in the suite was 1-D, so it went unnoticed. For rank >= 2 dimension 0
indexes rows, not elements: subscribing to row 0 of a ROWS x COLS dataset
produced the range `[0, 1)` — one element instead of the COLS-wide row
asked for — and the subscriber was served a fraction of its subscription
with no error. That is data loss, not coarse routing. The bounds are now
linearized against the dataspace's own extent (row-major, matching HDF5's
element ordering); `test/t_subvolume_nd.c` pins it by checking the *count*
of elements received, since the values that did arrive were correct and a
value-only check passes while most of the data goes missing.

The linearized range is the smallest flat span *containing* the selection's
bounding box, so a non-contiguous **subscription** yields a superset of what
was asked for. Deliberate: over-sending is inefficiency, under-sending is
data loss.

**A second, worse instance of the same assumption, on the write side.** A
push describes its payload as one flat range with the bytes contiguous from
`elem_start` — truthful only when the *write's* own selection is a single
contiguous flat run. It is for a whole-dataset write or a slab of leading
dimensions, but not for a strided one: a write of column 0 of a 4x8 dataset
was pushed as `elem_start=0 elem_count=4`, claiming flat elements 0..3 when
three of those were never written and the column's real values belong at
flat 0, 8, 16, 24. Unlike the subscription case that was *wrong values*, not
missing ones. Fixed by `H5VL__stream_space_flat_runs()`, which decomposes a
selection into maximal contiguous flat runs and pushes each separately —
no wire change, since the receiving queue already delivers one push at a
time with its own range. A whole-dataset or slab write stays a single push,
so the common path is unchanged. Point/irregular selections, or selections
exceeding the run cap, push nothing rather than something mislabelled: the
data is durably in the file regardless, and a missing push is recoverable
where silently wrong values are not. `test/t_strided_write.c` pins it by
checking every pushed element against whether the writer actually wrote that
flat index.

1-D element-range intersection shipped: a subscription's `H5Sencode2` bounds
(via `H5Sget_select_bounds()`, dimension 0) now thread through to the writer,
which intersects each subscriber's requested range against what it actually
wrote and pushes only the overlap — verified with a bounded subrange
subscription receiving exactly its requested elements, not the whole object.
See `vs_tr_writer_push_data()` in `src/tr_mercury.c`.

**Update: per-subscriber precision shipped too.** A subscription's
`H5Pencode2` DCPL (accepted and ignored since M8) now travels with the
subscribe RPC, and the writer re-filters that subscriber's own overlap slice
through the requested pipeline before sending it. HDF5 exposes no public
"apply this pipeline to a buffer" call outside ordinary dataset I/O, so the
mechanism is a throwaway `H5FD_CORE` (in-memory, no backing store) dataset
built with the subscriber's decoded DCPL: `H5Dwrite()` applies the filters,
`H5Dread_chunk2()` pulls the filtered bytes straight back out, and the
receiving side mirrors it exactly (`H5Dwrite_chunk()` to inject the already-
filtered bytes, ordinary `H5Dread()` to decode). Decision #3's "reuse the
existing filter pipeline instead of new compression code" holds literally —
there is no codec here, only HDF5's own. `src/tr_mercury.c` stays HDF5-free
via a registered `vs_tr_refilter_fn` callback rather than decoding property
lists itself.

Reversal is transparent: `H5Fget_subscribed_data()` always returns decoded
values, so a caller never sees filtered bytes and existing subscribers are
byte-for-byte unaffected. Verified end-to-end in `test/t_precision.c`: a
writer stores `/precise` **uncompressed** (`H5P_DEFAULT`), a subscriber
requests GZIP, and the push measurably shrinks **8000 → 48 bytes on the wire**
while decoding back to the exact original 2000 values — real measured wire
bytes, this section's exit gate asks for, from a pipeline the dataset itself
never had.

**Update: the exit gate's literal wording, demonstrated.** `test/t_precision_
dual.c` — three real OS processes, two simultaneous subscribers on `/precise`
(reader A: GZIP-9, reader B: none) served from one `end_step` — measures two
pushes sharing the same raw byte count but differing filtered sizes, proving
the "wire bytes scale with subscribed volume" claim with actual numbers, not
just per-subscriber-by-construction reasoning. Building it surfaced two real
bugs, both fixed at the source, not test artifacts:

1. `vs_subscribe_ult()`/`vs_reader_ack_ult()` answered "success" from *any*
   group member, not just the writer — invisible with one subscriber, but
   with two, reader B could file its subscription with reader A instead of
   the writer. Fixed with an `is_writer` guard in `src/tr_mercury.c`.
2. `vs_tr_stop()` called `ssg_finalize()` immediately after
   `ssg_group_leave()`/`ssg_group_destroy()`, with no settle window. A peer's
   SWIM ping already in flight to a just-departed member could still be
   dispatched to mochi-ssg's own `swim_dping_req_recv_ult()` after
   `ssg_finalize()` had nulled its runtime pointer, crashing the process
   (`Assertion 'ssg_rt' failed`). This was a real usage gap, not purely a
   third-party bug: mochi-ssg's own `tests/ssg-join-leave-group.c` sleeps
   between leave/destroy and finalize; `vs_tr_stop()` did not. Fixed by
   adding that same settle window — see its comment in `src/tr_mercury.c`.
   Stress-tested 20/20 crash-free afterward (reliably crashing before).

Still open, and why this test is real, built, and passing but *not* wired
into the default `ctest` run (`DISABLED TRUE` in `test/CMakeLists.txt`): a
separate, deeper residual where Mercury's single progress ULT can still block
inside an unbounded `connect()` in libfabric's TCP provider, reaching for an
already-departed peer during teardown. This never produces a wrong answer —
every run's actual exit-gate assertions pass first — and never crashes after
the fix above, only occasionally stalls process teardown past a normal test
timeout. The real fix belongs in libfabric/Mercury's own connection
handling, well outside this project.

Also still open: chunk-storage-granularity routing (`H5Sselect_intersect_
block` against `FilteredChunks`, which this connector has never actually
built despite being in the schema since M2). Because of that, re-filtering
always uses a single chunk spanning the whole pushed subrange rather than
honoring a chunk shape the subscriber's DCPL requested.

### M9 — Tools and the long tail · M

**Scope narrowed 2026-08-15.** An ADIOS2 interop bridge is **dropped** — not
deferred. h5py bindings are **postponed**, not abandoned.

In scope:

- `h5stream`, a real tool: list steps, tail a live stream, reorganize one
  into a static file. Note it does not exist yet in any form — the original
  wording ("promoted from test scaffolding") overstated the starting point,
  as there is no such scaffolding.
- The tool-compatibility matrix, documented honestly, `h5dump` included.
- Then predicate pushdown and onion-backed addressable step history.

**Exit gate.** `h5stream` follows a live writer end to end: lists steps as
they commit, tails new ones as they arrive, and reorganizes a running stream
into a static file that `h5dump` reads without complaint.

*Progress: `list`, `export` and `tail` have all landed and are tested. The
exit gate above is met.*

**A capture gap found by building it, unrelated to predicates and worse than
they were (2026-08-15).** M9's test needed a variable that *changes* step to
step — the first thing in this suite to write one dataset across several
steps through the handle it was created with. It wrote nothing. Capture
keyed strictly on a dataset wrapper still being a placeholder, i.e. created
in the step currently open and not yet materialized; once `end_step()`
replayed it, the wrapper went live and every later `H5Dwrite` through it
fell through to the under connector. Two silent consequences, both data
loss:

- the new step captured nothing, so no manifest entry, no push to any
  subscriber, and `/step/<n>/` was created empty (a 0-byte `.payload`);
- the write landed directly on the *earlier* step's copy of the object, so
  step history was not merely incomplete but retroactively wrong —
  `/step/0/temp` held whatever the last step wrote.

This is the most ordinary streaming pattern there is, and nothing covered it
because every existing test either wrote a single step or created a
differently-named object per step. Fixed by capturing a write to a live
dataset inside a step and synthesizing that step's own `DsetCreate` from the
object itself — its real datatype, extent and DCPL read back through the
under connector, so the step's copy is created exactly as the original was
and carries the whole extent even when the write covers part of it. That is
precisely what re-creating the dataset each step already produced, which is
the pattern that worked and the one decision #2 describes. Writes *outside*
a step still pass straight through, so M1's byte-identity promise is
untouched. `test/t_step_rewrite.c` pins both halves — each step holding its
own values, and a partial write landing at its own indices — checked through
the native connector, so it measures what is in the file rather than what
the connector believes.

#### Tool-compatibility matrix

Measured 2026-08-15 against HDF5 2.3's own tools, in all three states a
stream can be in. This is the "document it honestly, `h5dump` included" item,
and the honest answer has two halves rather than one.

| State | `h5ls` | `h5dump` | `h5stat` | Usable? |
|---|---|---|---|---|
| **Live** (writer holds it open) | `**NOT FOUND**` | error | error | **No** — use `h5stream tail` |
| **Finished** stream | opens | opens | opens | Yes, but see below |
| **Exported** (`h5stream export`) | opens | opens | opens | **Yes, fully** |

**Live streams: the tools do not work, and cannot.** All three fail, and this
is not a shortcoming of the tools or something a future release fixes: while
a writer holds an HDF5 file open, a second process cannot read it. `h5ls`
reports `**NOT FOUND**` on a file that is plainly on disk with content in it.
This is the constraint named at the top of this document, confirmed by
measurement rather than argument, and it is precisely why the connector
carries a transport — `h5stream tail` follows a live stream over
`H5Fwait_step_ready()` because no file-reading tool can.

**Finished streams: the tools open, but show the layout, not the data.** They
work in the sense that nothing errors — and are still not what a user wants.
A dataset the application wrote as `/temperature` appears as
`/step/0/temperature`, once per step that touched it, alongside `.manifest`
and `.payload` bookkeeping datasets. `h5ls -r` on a two-object, three-step
stream lists eleven entries. Everything is present and correct; finding the
current value of one variable means knowing which step last wrote it.

**Exported files: ordinary HDF5, no caveats.** `h5stream export` collapses a
stream to each object at its logical path, newest version, with no `/step`
groups and no bookkeeping. All three tools work normally, and so will
anything else that reads HDF5 — this is the answer for handing a stream to a
plotting script, an existing analysis pipeline, or a colleague.

The practical guidance, then: `h5stream tail` while it is running,
`h5stream export` once it is not, and the standard tools on the result.
Pointing `h5dump` at a stream directly is the one path that disappoints, and
it disappoints in both states for different reasons.

**Measured 2026-08-15 — `tail` cannot be built by polling the file.** The
obvious implementation, re-opening the file and watching for new `/step/<n>/`
groups, works against a finished stream and reports nothing at all against a
live one. The reason is not subtle and is not ours: while a writer holds the
file open, another process cannot read it. Plain `h5ls` on a stream mid-write
prints `**NOT FOUND**` even though the file is on disk with content in it.

That is this document's own "existing tools on a stream" constraint, met in
practice rather than in theory — and it is precisely why the connector has a
transport. `tail` has to be built on `H5Fwait_step_ready()`, which makes the
transport a hard requirement of the subcommand (`VOL_STREAM_NA` set, writer
publishing its group) rather than a preference: there is no other channel by
which a second process learns a step was committed.

The transport-based implementation has since shipped, once its test harness
was built correctly. The first attempt had the reader racing a fixed-length
writer: an ad-hoc writer emits its steps and exits, destroying its SSG group
before the reader finishes joining, so the reader reports "Group not found"
and gives up. That looks exactly like a broken tool and is entirely the
test's fault.

The fix was to invert the dependency rather than to tune timings — the
writer now keeps committing steps until the *reader* signals it is done, so
the reader may join whenever it manages to and steps will still be arriving.
That removes the attach race outright instead of making it less likely.
`test/t_h5stream_tail.c` asserts the tool observed at least three steps while
the writer was still running, which is precisely what the polling version
could not do; 10/10 stress runs, each seeing all three.

This replaces the previous gate, which was *"a viewer written entirely in
Python follows a live C or Fortran writer, subscribing to a subvolume, with
no user-written C glue."* That gate is precisely the h5py deliverable, so
postponing h5py left the milestone with nothing reachable to prove. It is
kept verbatim here and becomes M9's gate again if h5py is picked up — the
Python-viewer scenario is a strictly better demonstration than the tool
alone, which is why it is parked rather than discarded.

The narrowing also changes what the milestone is worth. The bridge and the
bindings were the two items aimed at other ecosystems; what remains is aimed
at this one — making a live stream inspectable with the tools an HDF5 user
already has, which is also what the "existing tools on a stream" constraint
in the opening section says is the real adoption risk.

## CI matrix

| Axis | Values | Why |
|---|---|---|
| HDF5 version | develop only (see note) | The documented failure mode; `H5VL_VERSION` is 3 and will move |
| MPI | MPICH · OpenMPI | Intercommunicator and dynamic-process behaviour differ in practice |
| Mercury NA plugin | `na+sm` · `ofi+tcp` · `ofi+verbs` | Where transport bugs live; shared-memory and TCP run on any CI box |
| rank shapes | 3→2 · 7→3 (uneven) | Coprime counts are where M×N projection bugs surface |
| encode round-trip | byte-order assertion (see note) | The manifest leans on HDF5's encoders; prove them across byte order |
| Spack env | pinned lockfile · latest deps | Pinned is reproducible; floating detects upstream breakage early |

**Cross-endian, without a big-endian machine (2026-08-15).** Reviewing the
format against this row turned up a real portability bug rather than
confirming one: the VL wire form's per-element length tag was `memcpy`'d from
a `uint64_t`, i.e. stored in the *host's* byte order. A file written on a
little-endian machine would have been unreadable on a big-endian one — every
tag byte-swapped, so lengths nonsense and the data either failing its bounds
check or decoding as garbage.

It was the only field at risk, which is why it was worth looking: FlatBuffers
is little-endian by specification, and the `H5Tencode`/`H5Sencode2`/
`H5Pencode2` blobs are HDF5's own portable encodings, so the hand-rolled tag
was the one place this project chose its own representation. It is now
written and read as explicit little-endian.

Note what this says about testing: the bug passed every round-trip check in
the suite, and would have on any single machine, because writer and reader
shared the host's byte order. `test/t_vl_roundtrip.c` therefore asserts the
*stored bytes* directly — it reads `.payload` back and requires the tag to be
little-endian — which fails on a regression regardless of the host it runs
on. That is real coverage of this row's intent; running an actual big-endian
leg (QEMU s390x) would additionally exercise HDF5's own encoders, and remains
open.

**HDF5 2.x is the only support target — this row is closed, not pending
(2026-08-15).** The original row called for develop, latest release, and
oldest supported, which read like unfinished work. It is not: 2.x is the
deliberate scope.

The code already reflects that and cannot easily do otherwise. The connector
uses `H5Tdecode2()` at 8 call sites — including the core manifest decode path
every replay runs through — and `H5Dread_chunk2()` at 5. Neither exists in
1.14.6, the latest release, checked against that tag's own headers rather
than inferred. A 1.14 job would not fail on some compatibility detail; it
would not compile.

So the row now says develop, matching the `build` job's own comment. Nothing
here is blocked on CI work; supporting 1.14 would mean writing a
compatibility layer for those APIs *and* changing what the project targets.
| sanitizers | ASan · UBSan · TSan | A progress thread plus a queue is the classic place for races |

## Residual risks

**The manifest inherits HDF5's encoding compatibility rules.** Leaning on
`H5Tencode` and `H5Sencode2` is the right trade, but it couples the wire format to
them, and `H5Sencode2` already changed once to widen selections to 64-bit. The
schema records `hdf5_version` from the first commit and CI runs a cross-endian
encode round-trip, so a future change is *detectable*. **Update:**
`H5VL__stream_replay_manifest()` now reads `hdf5_version` back and compares it
against the running library's own major.minor (ignoring the release/patch
digit, which HDF5's versioning policy never lets change encoding format) before
either decode call runs, refusing with a clear `stderr` diagnostic on a
mismatch rather than risking whatever `H5Tdecode2()`/`H5Sdecode()` do with
bytes from an incompatible encoder. Verified two ways: the full existing test
suite (writer and reader always share one build here, so this exercises the
no-op/compatible path end-to-end) and the comparison arithmetic itself checked
in isolation against the actual 1.10→1.12 precedent. **Not verified
end-to-end:** the mismatch-detected branch itself, which needs a second real
HDF5 build (a different minor series) to trigger for real — not available this
session; the CI matrix's own "HDF5 version" axis builds the *connector*
against different HDF5 series one at a time, it does not yet write with one and
read with another in the same test.

**Manifest metadata was fully re-encoded every step, with no dictionary.**
`H5VL__stream_build_manifest()` calls `H5Tencode`/`H5Sencode2` (and
`H5Pencode2` for create-kind entries) on *every* pending entry on *every*
step. `dcpl_enc` was never actually part of this cost for `DsetWrite` (its
`dcpl_id` is always `H5I_INVALID_HID` — only `DsetCreate`/`Attr` carry one,
and those happen once per object, not once per step); `type_enc` was the
real, live cost, since a `DsetWrite` re-sent it in full every step even
though a dataset's type never changes after creation. `space_enc` is left
alone — a write's selection legitimately varies step to step and is not
part of this fix.

**Update:** `type_enc` is now cached per path, on both the writer side
(`H5VL__stream_type_cache_lookup`/`_upsert`, omitting the field when it is
byte-identical to the last one sent for that path) and the replay side
(a mirror cache, resolving an omitted field back to real bytes — both sides
run in the same process, so no cross-process synchronization is needed).
No schema change: an omitted flatbuffers vector field decodes as a 0-length
result via `flatbuffers_uint8_vec_len()`, a sentinel no valid `H5Tencode()`
output can produce, so this is unambiguous and fully backward compatible.
Verified in `test/t_manifest_cache.c` by comparing the *persisted*
`/step/<n>/.manifest` dataset's on-disk size across three steps writing the
same path: step 0 (create + first write, nothing to omit) is measurably
larger than steps 1–2 (repeat writes, `type_enc` omitted), and steps 1–2
land at the identical, stable size — a black-box proof the omission is
real, not just a code-reading claim, plus a correctness check that the
cached path never changes what actually lands in the file.

**Existing tools on a live stream.** See the constraint section above.

**VL and reference support (Decision #4) were never implemented, not just
deferred.** The residual-risk plan here was "ship them behind a property
defaulting to off" if M2 slipped — but no such property exists, and neither
`H5Rget_obj_name`/`H5Rcreate_object` (reference name translation) nor
`H5Dvlen_get_buf_size`/`H5Treclaim`-based deep serialization exist anywhere in
`src/`. The capture path is datatype-agnostic: it `memcpy`s whatever buffer
`H5Dwrite`/`H5Awrite` was given straight into the pending entry. For a
variable-length datatype, that buffer is an array of `{len, pointer}`
(`hvl_t`) structs, and the pointers are only valid in the writer's own
process memory — capturing them verbatim and replaying them later (a
different `H5Dwrite` call, against a different handle, however much later
`end_step()` runs) reads through stale pointers rather than the data itself.
This was a live correctness gap, not just a latency or cost concern: writing
VL data through this connector was unsafe, and nothing detected or rejected
it. **Update:** option (b) has landed —
`H5VL__stream_type_unsafe_to_capture()` rejects a `DsetWrite`/`Attr` capture
outright (return -1, clear HDF5 error) for `H5T_VLEN`, `H5T_REFERENCE`, and
variable-length-string types, checked recursively through `H5T_COMPOUND`/
`H5T_ARRAY` members so a struct or array *containing* one of these is caught
too. A rejected write does not poison the rest of the step — a plain write
to a different object in the same step still replays correctly (verified in
`test/t_vl_reject.c`, the exit gate for this fix). Real deep-serialization
(a) is still not built; this closes the silent-corruption failure mode, not
the missing feature. **Update 2026-08-14: (a) has now landed too** — see the
VL deep-serialization paragraph below. Reference translation, once built, should happen at
write-time (intercepting `H5Rcreate_object`/`H5Rcreate_region`) rather than
by walking the whole step's references synchronously inside `COMMITTING` —
the latter would stall the collective barrier in proportion to reference
count.

**VL deep-serialization landed 2026-08-14 — the last piece of Decision #4.**
A VL buffer is not self-contained (`H5T_VLEN` is an array of `hvl_t`
{len, pointer}; a VL string an array of `char *`), and this connector defers
the real write to `end_step()`, by which point the application is entitled to
have reclaimed all of it. So capture now follows those pointers immediately
and copies what they point at (`H5VL__stream_vl_serialize()`), and replay
rebuilds real pointers into fresh allocations before writing
(`H5VL__stream_vl_deserialize()`, released right after the write since HDF5
has copied by then). The wire form is, per element, `[uint64 tag][tag-1
bytes]`, where `tag == 0` marks a NULL pointer — the +1 bias is what keeps
NULL distinguishable from a legitimately empty sequence or `""`, which HDF5
round-trips differently.

Wired on both the dataset and attribute capture/replay paths. `test/t_vl_
roundtrip.c` covers ragged lengths (0, 1, many) and the empty string, checks
what actually landed through the *native* connector, and — the load-bearing
case — frees every application buffer **before** `end_step()` runs, so a
regression back to capturing pointers rather than bytes would read freed
memory and fail there.

Still rejected, deliberately: a VL *nested* inside a compound or array, and
a VL whose base type is itself variable-length. Deep-serializing those means
walking a nested layout member by member, which this does not do.
`test/t_vl_reject.c` was narrowed to assert exactly that remaining boundary
(it previously asserted the interim "reject all VL" behavior).

**Measured 2026-08-14 — reference support is feasible, but Decision #4
understates what it takes.** Three probe programs run against the real
connector (not code reading) establish:

1. `H5Rcreate_object(fid, "/target", …)` for a target created in the **same
   step** fails outright — `object 'target' doesn't exist`. Deferred writes
   mean the target is genuinely not in the underlying file until
   `end_step()` replays the manifest.
2. The same call for a target in a **prior, committed** step also fails, for
   a different reason: steps land group-based at `/step/<n>/`, so the
   application's logical `/target` is physically `/step/0/target` (confirmed
   with `h5dump -n`).
3. The same call given the **physical** path works, and `H5Rget_obj_name()`
   returns `/step/0/target` — so the HDF5 mechanism this decision rests on
   is sound.

The consequence: name translation is the easy half. `H5VL_stream_object_
specific()` is currently a pure passthrough, so making the natural call
`H5Rcreate_object(fid, "/target")` work needs a logical→physical path
translation layer in the object-lookup path; capture would then store the
logical path (step prefix stripped) so a reader rebuilds against its own
layout. And the same-step case is a real architectural limitation, not an
oversight — it needs either a documented "targets must come from an earlier
step" rule, or deferred reference resolution during `COMMITTING`, after
objects materialize but before the manifest closes.

**Status: landed.** `H5VL__stream_resolve_physical_path()`, writer-side
`path_index` population, and the `H5VL_OBJECT_LOOKUP` translation branch
make `H5Rcreate_object(fid, "/target")` work with the logical path
(`test/t_ref_path.c`, which also asserts that a same-step target is still
refused — the documented "earlier step" rule above). Capturing a dataset
*of* references and replaying it now works too (`test/t_ref_roundtrip.c`):
`H5VL__stream_type_unsafe_to_capture()` no longer rejects a *top-level*
reference, and an ordinary memcpy — the same path every other fixed-size
type already uses — carries it through capture and replay correctly.

Two capture designs were tried and abandoned before landing on the one that
works, and what each ruled out is worth recording, because the eventual fix
is the opposite of what the danger looked like it required.

*Attempt 1 — translate at capture with `H5Rget_obj_name()`.* Crashes
(`H5G_rootof: Assertion 'f->shared' failed`). Resolving a reference goes
through the `hid_t` an `H5R_ref_t` caches internally, which is the
application's own file id — i.e. this connector — so the call re-enters the
library from inside a VOL callback. Isolated with probes: the identical call
at application level, followed by a create and an `end_step` replay, is
fine; only the call from within a callback breaks.

*Attempt 2 — cache token → logical path, no HDF5 call at capture.* The
connector already sees the target's logical path at `H5Rcreate_object()`
time, as the name arriving in the `H5VL_OBJECT_LOOKUP` branch, and the token
that lookup returns is what ends up inside the reference (an `H5R_ref_t`
begins with its token, for every reference flavour). Capture becomes a pure
table lookup, with no re-entrant HDF5 call anywhere. The manifest payload
came out holding the right logical names, and nothing else regressed — but
`end_step()` still crashed at the same assertion, now at the very first
`ensure_group()` of the replay, with the *native* file object already
carrying a NULL `shared`. So the fault was never the re-entrancy attempt 1
found; it was something about translation itself.

*Attempt 2's crash, root-caused afterwards (M8.5.1 turned it up).* Its replay
half called `H5VLwrap_register(file_under_object, H5I_FILE)` to get an
`hid_t` for `H5Rcreate_object()`, then `H5Idec_ref()` to release it. That id
**owns** the object it wraps, so releasing it closed the underlying file
itself — which is exactly the `f->shared == NULL` seen at the next
`ensure_group()`. Nothing to do with translation, or with a reference
entry's payload being shorter than `n_elem × H5Tget_size()`. Worth stating
plainly because the same trap bit the M8.5.1 fast path independently: to
reach an under-VOL object from inside a callback, do not take an `hid_t` at
all — use the connector's own optional operations, which take the object
directly.

*What actually unblocked it: don't translate at all.* A raw memcpy diagnostic
— accept the write, copy the bytes verbatim, exactly like any other
fixed-size type — round-tripped correctly on the first try. The "unsafe"
premise from the original guard doesn't hold for the only case reachable
through this connector's own API: `H5Rcreate_object()` builds a reference
relative to its `loc_id`'s own file, so an application cannot name a
genuinely foreign file without a `loc_id` in *that* file — which would not
route through this connector at all. For a same-file reference
(`H5R_OBJECT2`/`H5R_DATASET_REGION2`/`H5R_ATTR`, confirmed via
`H5Rget_type()`), the cached filename pointer inside the private
`H5R_ref_priv_obj_t` is NULL — read directly at the byte level against
HDF5's own (private, `H5Rpkg.h`) layout. Nothing to dangle, so memcpy is
exactly as safe as for any other scalar. `H5VL__stream_type_unsafe_to_
capture()` now takes a `top_level` flag: a reference at the top level of a
write is exempt; one nested inside a compound or array still is not, since
translating that would mean rewriting the buffer member by member, which
nothing here does — `test/t_ref_roundtrip.c` asserts that boundary
explicitly, not just the positive case.

**The dependency risk is quality, not quantity.** Criteria before adding
anything: actively maintained, deployed at target facilities, Spack-installable,
bounded scope, stays out of the wire protocol. Mercury, Argobots, Margo and
flatcc pass all five. What needs enforcing is a pinned Spack environment checked
in from M0.

**Plugin signatures.** An HDF5 built with `-DHDF5_REQUIRE_SIGNED_PLUGINS=ON`
refuses to load an unsigned connector, with an error that reads like corruption
rather than a policy refusal. This affects the plugin path only — and the M0 exit
gate uses the plugin path. See [`plugin-signing.md`](plugin-signing.md).
