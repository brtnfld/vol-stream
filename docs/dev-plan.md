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
exactly. (`FilteredChunks` itself is schema-defined but not yet built — see
M8.5's status in `project_m8_status.md` — so this rule applies once that
capture path exists, not to the current `Raw`-only implementation.)

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

1-D element-range intersection shipped: a subscription's `H5Sencode2` bounds
(via `H5Sget_select_bounds()`, dimension 0) now thread through to the writer,
which intersects each subscriber's requested range against what it actually
wrote and pushes only the overlap — verified with a bounded subrange
subscription receiving exactly its requested elements, not the whole object.
See `vs_tr_writer_push_data()` in `src/tr_mercury.c`. Chunk-storage-granularity
routing (`H5Sselect_intersect_block` against `FilteredChunks`, which this
connector has never actually built despite being in the schema since M2) and
per-subscriber precision (re-filtering) remain open.

### M9 — Tools, bindings, and the long tail · M

`h5stream` promoted from test scaffolding to a real tool: list steps, tail a live
stream, reorganize into a static file. h5py bindings. Document the
tool-compatibility matrix honestly, `h5dump` included. Then predicate pushdown,
onion-backed addressable step history, and an ADIOS2 interop bridge.

**Exit gate.** A viewer written entirely in Python follows a live C or Fortran
writer, subscribing to a subvolume, with no user-written C glue.

## CI matrix

| Axis | Values | Why |
|---|---|---|
| HDF5 version | develop · latest release · oldest supported | The documented failure mode; `H5VL_VERSION` is 3 and will move |
| MPI | MPICH · OpenMPI | Intercommunicator and dynamic-process behaviour differ in practice |
| Mercury NA plugin | `na+sm` · `ofi+tcp` · `ofi+verbs` | Where transport bugs live; shared-memory and TCP run on any CI box |
| rank shapes | 1→1 · 7→3 · 64→5 | Coprime counts are where M×N projection bugs surface |
| encode round-trip | cross-endian pair | The manifest leans on HDF5's encoders; prove them across byte order |
| Spack env | pinned lockfile · latest deps | Pinned is reproducible; floating detects upstream breakage early |
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
the missing feature. Reference translation, once built, should happen at
write-time (intercepting `H5Rcreate_object`/`H5Rcreate_region`) rather than
by walking the whole step's references synchronously inside `COMMITTING` —
the latter would stall the collective barrier in proportion to reference
count.

**The dependency risk is quality, not quantity.** Criteria before adding
anything: actively maintained, deployed at target facilities, Spack-installable,
bounded scope, stays out of the wire protocol. Mercury, Argobots, Margo and
flatcc pass all five. What needs enforcing is a pinned Spack environment checked
in from M0.

**Plugin signatures.** An HDF5 built with `-DHDF5_REQUIRE_SIGNED_PLUGINS=ON`
refuses to load an unsigned connector, with an error that reads like corruption
rather than a policy refusal. This affects the plugin path only — and the M0 exit
gate uses the plugin path. See [`plugin-signing.md`](plugin-signing.md).
