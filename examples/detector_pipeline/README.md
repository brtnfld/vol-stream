# detector_pipeline: the structure travels with the data

The use case behind **RFC-VOLSTREAM-2026-001 section A.2**, "What the Domain
Already Deploys."

At PETRA III beamline P02.2, a twelve-module LAMBDA detector streams into
ASAP::O, and the pipeline hanging off it has the shape every photon-facility
detector pipeline has: something archives at full fidelity, something shows a
live view, something processes one panel, something decides which frames are
worth keeping, and something watches what is happening right now and feeds
that back into running the experiment -- ASAP::O's own stated purpose
includes exactly this: "online analysis, monitoring, and feedback" for
"automated experiments." That part is not in dispute and this example does
not claim to improve on it.

What this example is about is narrower, and it is the one architectural
difference A.2 identifies. In the P02.2 integration the detector PC sends
image data to ASAP::O as a data stream *and separately* sends the NeXus
structure as stream metadata, and a distinct service -- the ASAP::O NeXus
writer -- consumes both and produces the file. The structure of the HDF5
object travels beside the data, as a description of itself, for a third
component to reassemble.

A VOL connector does not need any of that, because HDF5 can already serialize
its own data model. `H5Tencode`, `H5Sencode2` and `H5Pencode2` mean the step
manifest carries the datatype, dataspace and filter pipeline *as the library's
own bytes*, in the same envelope as the payload. A consumer reconstructs them
with `H5Tdecode2`/`H5Sdecode`. There is no second channel to keep in sync and
no writer service to reassemble anything.

`H5Fget_stream_schema()` is that claim made executable, and this is the first
example in the repo to use it.

## The two programs

**`detector_writer`** is the acquisition. It writes a NeXus-shaped layout --
`/entry/data/data` as a 3-D int32 stack with an unlimited leading dimension
and one chunk per frame, plus `pixel_mask` (uint8) and `flatfield` (float32)
under `/entry/instrument/detector`. Four modules, each writing its own
hyperslab into one logical frame.

The important thing is what it does **not** do: it never describes its layout
to anybody. No schema message, no structure channel, no side agreement. It
calls `H5Dcreate2()` and `H5Dwrite()`, and that is the entire interface.

**`pipeline_consumer`** is one binary with six modes, five of them the P02.2
components and one of them the point:

| mode | role at P02.2 | what it asks for |
|------|---------------|------------------|
| `discover` | -- | `H5Fget_stream_schema()` and nothing else |
| `archive` | the NeXus writer | full fidelity, no narrowing |
| `viewer` | the live view | `H5Fsubscribe_type(int16)` + a deflate DCPL |
| `analysis` | the stitcher's per-panel input | one module's row band |
| `hitfinder` | the veto / Cheetah role | `H5Fsubscribe_predicate(GT)` |
| `monitor` | monitoring / feedback for automated experiments | the same predicate, paired with `H5Fwait_step_ready()` |

## What makes this different from `narrowing_demo`

`examples/narrowing_demo` already shows one write narrowed per subscriber.
The difference here is **where the shapes come from**.

`narrow_subscriber` builds its dataspace out of `NARROWING_NELEM`, a constant
it shares with the writer through a header. As `test/t_schema.c`'s own comment
notes, every example and test in this repo did that, by construction, because
before `H5Fget_stream_schema()` there was no other way -- `H5Fsubscribe()`
takes a dataspace, and a reader had to invent one.

`pipeline_consumer` does not. The only things it takes from
`detector_common.h` are the filename, the timeouts, and the image path *as a
string to match on* -- the way a real NeXus consumer keys off an `NXdata`
attribute. Never a dimension, never an element count, never a datatype. Every
buffer it sizes and every selection it builds comes from the ids discovery
handed back. Delete the geometry macros from `detector_common.h` and
`pipeline_consumer.c` still compiles.

That is the difference between a purpose-built monitor and a generic
consumer, and it is what a visualization tool needs in order to present a
variable list before a user picks anything from it.

## Running it

```
cmake --build build          # a build dir with VOL_STREAM_HAVE_MERCURY on
examples/detector_pipeline/run_pipeline.sh build/examples/detector_pipeline
```

That starts the writer and all five consumer roles at once and prints what
each measured. Or by hand:

```
# terminal 1
VOL_STREAM_NA=ofi+tcp VOL_STREAM_DEBUG_REFILTER=1 ./detector_writer

# terminals 2-6
VOL_STREAM_NA=ofi+tcp ./pipeline_consumer archive
VOL_STREAM_NA=ofi+tcp ./pipeline_consumer viewer
VOL_STREAM_NA=ofi+tcp ./pipeline_consumer analysis 2 4
VOL_STREAM_NA=ofi+tcp ./pipeline_consumer hitfinder
VOL_STREAM_NA=ofi+tcp ./pipeline_consumer monitor
```

`detector_writer [nframes] [delay-ms]` -- defaults 8, 500.
`pipeline_consumer <mode> [module] [nmodules] [max-pushes] [step-timeout-ms]`.

The single most direct thing to run is `pipeline_consumer discover` against a
live writer. It prints every path, datatype and extent the stream carries and
then exits, having been told none of it.

## Cadence, stated narrowly

`pixel_mask` and `flatfield` are written once, inside step 0, while frames
arrive every step. All three appear in the discovered schema.

Be careful what this demonstrates. Independent cadence *on its own* is
expressible in other systems too -- an ASAP::O data source can open separate
streams for separate things, and its dataset-substream mechanism is the part
that assumes a common cadence. The difference A.2 actually claims is narrower:
independent cadence **with an explicit dependency**, meaning a consumer can
resolve a given frame against the correct calibration. This example shows the
cadence. It does not by itself demonstrate the dependency, which is decision 1
in the RFC's design-decisions section.

## `monitor`: checking status and driving the next experimental step

This is the role ASAP::O's own introduction names directly: "online analysis,
monitoring, and feedback" in support of "automated experiments." The other
four consumers each answer "what does this frame look like." `monitor`
answers a different question -- "is this acquisition worth continuing" --
and prints an explicit decision at the point it changes its mind, the way a
real facility's automation would act on it.

**The scene it watches is not the alternating one.** `detector_frame_is_hit()`
now gives the first `DETECTOR_HIT_FRAMES` frames signal and every frame after
that none -- a decaying acquisition, not an alternating one (see
`detector_common.h`). The referent is real: RFC appendix A cites serial
femtosecond crystallography, where a crystal's useful diffraction window is
short and facilities veto once it closes rather than keep collecting past it.
This changes nothing for `archive`/`viewer`/`analysis`/`hitfinder` -- they
never depended on the specific temporal pattern, only on some frames being
hits and some blank -- but it is what lets `monitor`'s two decisions both
actually fire in the default eight-frame run instead of one of them being
merely described.

**Why it needs a different loop than the other four.** `archive` and
`hitfinder` drain a push queue: `H5Fget_subscribed_data()` timing out just
means "no push arrived," which does not distinguish "this frame had no
signal" from "the writer has not reached the next frame yet." That
ambiguity does not matter to a consumer that only cares what it delivers.
It matters a great deal to one whose job is *checking status*. So `monitor`
pairs two calls per frame: `H5Fwait_step_ready()` first, which reports that
the writer's transport announced a newly committed step regardless of
whether any subscription matched it, and only once that confirms a step
really happened does it drain `H5Fget_subscribed_data()` to see whether the
predicate matched. The drain does not block: the writer delivers all of a
step's pushes before it announces the step (pinned by
`test/t_step_grouping.c`), so an empty queue right after a confirmed commit is
a real "no signal this frame," not silence that could mean anything. A hit
frame arrives as several pushes -- one per contiguous run of matching pixels
across the four module writes -- so the monitor drains all of them before
scoring the step, and if it has fallen behind it holds the next step's first
push over for that step instead of discarding it.

**The decision it drives.** Two thresholds, both in `detector_common.h`:
`DETECTOR_MONITOR_GOOD_HITS` hits observed declares signal established;
`DETECTOR_MONITOR_MISS_STREAK` consecutive misses *after* that declares it
lost. Against the default scene this prints a `DECISION -- signal
established` line around frame 2 and a `DECISION -- signal lost` line
around frame 5 -- both genuinely triggered by what the writer sent, not
scripted separately.

**What this is not.** `monitor` does not call into any real control system
-- no Tango, no EPICS, no Bluesky. What it prints is what a real deployment
would *do* with the decision, clearly labeled as such. This example
demonstrates the status-checking mechanism honestly; the automation hook is
the integration point it exists for, not something it implements.

**A documented capability this does not exercise.** vol-stream's own header
names a "monitoring/latest-only reader" pattern -- jumping to the newest
frame by logical id via `H5Fbegin_logical_step()` rather than draining
sequentially (see `H5Fset_stream_queue_policy()`'s doc comment), which is
also what exempts such a reader from queue-policy backpressure entirely.
That would be the more scalable design for `monitor` at real frame rates:
sample the latest, skip whatever was missed, never fall behind. It is not
built here. Verifying exactly how it interacts with the subscription push
queue needs a machine that can actually run this connector with the Mochi
stack (mercury, margo, argobots, flock) present, which the one this was
written on does not have -- left as a documented gap rather than a guess.

## Honest notes

- **One writer process, not M ranks.** The four modules are four
  `H5Dwrite()` calls into one frame from a single process. That produces the
  per-panel geometry `analysis` subscribes to, but it is not the M×N case --
  that is `test/t_parallel.c`. This example is about the consumer side.
- **A row band is the favourable geometry.** Panels stacked in the slow
  dimension make a single panel contiguous. A *column* band of the same area
  is not, and is exactly the case RFC appendix A's finding F5 shows is still
  over-sent -- 514 runs against a 256-run cap. Do not read `analysis`'s
  numbers as a general claim about ROI subscriptions.
- **The hitfinder stays inside the documented run cap.** The scene has 24
  bright spots per hit frame, against `VS_TR_MAX_PRED_RUNS` of 64, so the
  predicate is served exactly. Finding F4 in the RFC shows what happens past
  that cap: ~160 scattered pixels coalesce to a 98% send. This demo is
  deliberately on the good side of that cliff, and the cliff is real.
- **The viewer cannot see its own compression.** `H5Fget_subscribed_data()`
  always hands back decoded values, so the viewer's reported byte count is the
  decoded size. The actual wire bytes appear only in the writer's own
  `refilter` log line under `VOL_STREAM_DEBUG_REFILTER=1`. That is RFC
  appendix A's finding F7, and it applies here unchanged.
- **This is not a benchmark.** No timing, no sustained rate, no comparison
  against ASAP::O or anything else. It demonstrates a capability;
  `test/b_detector_narrowing.c` is where the measured numbers live.
- **`monitor`'s decision thresholds are small on purpose, not because that is
  the right size for a real deployment.** `DETECTOR_MONITOR_GOOD_HITS` and
  `DETECTOR_MONITOR_MISS_STREAK` are set to 2 specifically so both decisions
  fire inside an eight-frame demo run. A real facility would size these
  against real frame rates and real false-positive costs; nothing here
  argues for these particular numbers.
- **Built, not run.** CI's transport job compiles and links every mode; no
  test executes this example. The ordering `monitor` relies on -- a step's
  pushes queued before its step-ready arrives -- is now pinned by
  `test/t_step_grouping.c` rather than reasoned from the API docs. Two bugs
  in `monitor`'s loop were found by review, not by running it, and fixed: it
  read one push per step where a hit frame produces several, and it
  discarded a later step's push instead of holding it. Its decision output
  against the default scene has still not been observed.
