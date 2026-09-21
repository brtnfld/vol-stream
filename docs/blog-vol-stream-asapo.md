# Blog post: vol-stream and ASAP::O

Working notes for a post comparing vol-stream to ASAP::O, the streaming
platform deployed across multiple PETRA III beamlines. Not drafted yet. The
groundwork exists in two places and this file is the plan connecting them:

- **RFC-VOLSTREAM-2026-001 section A.2**, "What the Domain Already Deploys"
  (`sections/remote-sensing.tex`, `\label{sec:rs-asapo}`) — the argument.
- **`examples/detector_pipeline/`** in this repo — the working demonstration
  of A.2's central claim.

## The angle

Lead with the one-channel/two-channel difference. It is concrete, it diagrams
well, and the underlying insight travels beyond this one comparison:
**HDF5 can serialize its own data model, so streaming it does not need a
schema sidecar.**

At PETRA III P02.2 the LAMBDA detector PC sends image data to ASAP::O as a
data stream *and separately* sends the NeXus structure as stream metadata, and
a third component — the ASAP::O NeXus writer — consumes both to produce the
file. The structure of the HDF5 object travels beside the data, as a
description of itself, for another service to reassemble.

`H5Tencode` / `H5Sencode2` / `H5Pencode2` remove the need for any of that: the
step manifest carries datatype, dataspace and filter pipeline as the library's
own bytes, in the same envelope as the payload, and a consumer reconstructs
them with `H5Tdecode2` / `H5Sdecode`. `H5Fget_stream_schema()` is that claim
made executable — a consumer discovers every path, type and extent with no
side agreement beyond a filename.

Secondary point, if there is room: the archival path. P02.2 runs
ASAP::O → NeXus writer → data storage → ASAP::O reader → back into ASAP::O,
five components to serve one detector's live and archival consumers. The onion
VFD already models an append-only numbered revision history, so a step is both
a live stream element and a re-openable revision from one write path.

## Three things to settle before drafting

**Stance.** An RFC section and a public post comparing yourself to a named
production system at a named facility are different acts. ASAP::O's developers
will read it. A.2's hedges — "production software serving real beamtime,"
"this is scoping, not a proposal" — need to survive into the post. The
temptation when compressing for a blog is to drop exactly those lines.

**Timing.** DESY has convened a committee of developers and beamline
scientists to define requirements, review other facilities' solutions, and
build a testbed, with a final report expected **November 2026**. Before,
during, and after that window are three different posts. Before reads as input
to an open evaluation; after reads as a response to its conclusion.

**The weak spot.** vol-stream has no Python path. `h5py` has no binding for
`H5VLfile_optional_op()`, so a Python consumer cannot call `H5Fsubscribe()` at
all. ASAP::O ships C++ *and* Python producer and consumer APIs as a headline
feature and sits under a Python-first control stack (Tango, Sardana, Bliss).
Naming this early costs a paragraph and buys credibility; omitting it invites
the first comment to be the rebuttal. The standalone Python subscriber is
scoped in the RFC (`\label{sec:pysub}`) and is the recommended next
deliverable, but it is not built.

## Results the post needs

### The question is "can it do this, and do it well" — not "is it faster"

Benchmarking a research connector against production software deployed across
a facility is not a meaningful contest, and framing the post as one would
invite a scoreboard nobody should want. The claim worth supporting is narrower
and more defensible: **vol-stream can serve the same pipeline shape, it does so
competently, and it gets there with fewer moving parts.**

So the numbers are evidence of capability and of reasonable cost, not of
victory. Nothing in the post should read as "% better than ASAP::O." State
ASAP::O's side architecturally from published sources and never invent a
number for it — a fabricated baseline is the one thing that would discredit
the whole piece.

### The primary deliverable is a coverage table, not a speed number

For each thing the P02.2 pipeline does, what serves it here:

| P02.2 component | vol-stream equivalent | status |
|---|---|---|
| Image data stream | step payload | built |
| NeXus structure as stream metadata | carried in the step manifest | built — no second channel |
| ASAP::O NeXus writer service | the connector's own write path | built — no separate service |
| Stitcher's per-panel input | selection-exact subscription | built |
| Live view | `H5Fsubscribe_type` + filter pipeline | built |
| Veto / hit finding | `H5Fsubscribe_predicate` | built, with caveat **F4** |
| Archive ← storage ← reader loop | step-addressable history, one write path | built |
| Facility namespace (beamtime/data source), durable indexed log | — | **not in scope** |
| Python producer/consumer API | — | **not built** |

Filling that table honestly, including the last two rows, is worth more to the
post than any latency figure.

### Supporting measurements

In descending order of how much the thesis depends on them.

1. **Schema-channel cost.** Speaks to the lead claim, and nothing currently
   measures it. How many bytes the encoded schema is, and how many times it is
   republished over N steps — it should republish on change, not per step. The
   point is not that it is small but that it is **not a per-step channel and
   not a separate service**.

   Gotcha: `H5Fget_stream_bytes_pushed()` **cannot** supply this. It is
   payload only and explicitly excludes "RPC framing, metadata and the encoded
   type/space blobs," which is exactly the quantity in question. This needs a
   new counter or a debug line.

2. **Per-consumer wire bytes on P02.2 geometry.** The four roles in
   `examples/detector_pipeline/` — archive, viewer, analysis, hitfinder — each
   as a fraction of the archive baseline. The claim is proportionality: each
   consumer pays for what it asked for, from one `H5Dwrite()`.

3. **Fidelity.** The archive consumer receives bit-identical data, and the
   file left behind is a valid NeXus-shaped HDF5 file. "Does it well" means
   correctness first; `b_detector_narrowing.c` already verifies values rather
   than only counting bytes, and the post should say so.

4. **Cost of the fan-out.** Writer-side commit latency, archive alone versus
   all four roles attached. Report this as the price of the capability, not as
   a win — appendix A's **F3** already shows it rises steeply on
   incompressible frames, and that belongs in the post too.

### Machinery to reuse, not rebuild

`test/b_detector_narrowing.c` already measures (2) and (3) for a single
EIGER2-class module with ten subscribers. It captures real wire bytes by
parsing the writer's `VOL_STREAM_DEBUG_REFILTER` log, because
`H5Fget_subscribed_data()` always hands back decoded values — an application
cannot observe its own compression, which is finding **F7** in the RFC's
appendix A and is itself worth a sentence in the post.

### Caveats that must travel with any numbers

From the RFC's appendix A, and they apply unchanged here:

- **F4** — predicate pushdown collapses on realistic sparsity. A run cap of 64
  turns ~160 scattered non-zero pixels into a 98% send. `detector_pipeline`
  uses 24 spots per frame, deliberately inside the cap. Do not present the
  hitfinder's numbers without saying so.
- **F5** — a *column* ROI is still over-sent (514 runs against a 256-run cap).
  `detector_pipeline` stacks panels in the slow dimension, which is the
  favourable geometry.
- **F6** — the zero-copy fast path never fires on a streaming append, so
  acquisition-time compression is redone per subscriber.
- Scenes in the existing benchmark are synthetic. Validating against a
  recorded EIGER or AGIPD run is the single change that would most increase
  confidence.

## Environment

The Mochi stack (mercury, margo, argobots, flock ≥ 0.8.0, margo ≥ 0.24.2) is
required and is not resolvable on the laptop this was planned on. All of
`examples/` and the transport-dependent tests are gated behind
`VOL_STREAM_HAVE_MERCURY`. Run the measurements on a machine that has it.

## Sources

- Yu, Yuelong. *Streaming High Throughput Detector Data Using ASAPO.* DESY
  (FS-EC), 22 September 2026. Venue to confirm.
- ASAP::O documentation — <https://asapo.pages.desy.de/asapo/>
- ASAP::O, Comparison to Other Solutions —
  <https://asapo.pages.desy.de/asapo/docs/compare-to-others/>
- ASAP::O source — <https://gitlab.desy.de/asapo>
