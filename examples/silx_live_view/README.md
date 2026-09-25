# silx_live_view: the control-room live view, pushed instead of polled

The live viewer beamlines run today is a silx widget. At MAX IV, "a simple
raw data live-viewer based on SILX is provided for all detectors and cameras.
It works by making HTTP requests at 1 Hz to the stream-receiver for the latest
frame." The receiver also gained a downsampling option, because for large
cameras the data rate "even at 1 fps, would saturate the bandwidth of a client
computer in the control room" (Bell et al., arXiv 2601.05901). ESRF's Flint,
the BLISS live display, is also built on silx.

`silx_live_view.py` is that viewer fed by vol-stream. Here is what changes:

- **Pushed, not polled.** The writer pushes each frame when its step commits.
  The viewer redraws the newest frame on a timer (`--fps`, default 10) and
  drops the backlog, so a slow window only skips frames.
- **Reduced for this viewer only.** `--dtype int16` (the default) has the
  writer convert frames before sending, for this subscriber alone, which
  halves the bytes of an int32 detector. The archive and other consumers still
  get full fidelity from the same write.
- **Cannot slow acquisition.** The viewer never acks, so the writer never
  counts it as a reader that is behind (`H5Fset_stream_queue_policy()`).
- **Discovered, not configured.** The frame stack is the first rank-3 numeric
  dataset in the writer's schema, unless `--path` names one. There is no shape
  or type constant shared with the writer.

The writer is `examples/detector_pipeline/detector_writer`, unchanged except
for its third argument: the number of consumers to wait for, where 0 means
start at once and let consumers join late.

## Running it

Needs a build with `-DVOL_STREAM_BUILD_PYTHON=ON` and the transport. The
window also needs silx and a Qt binding (`pip install silx PyQt5`).

```
examples/silx_live_view/run_live_view.sh build 200 100            # 200 frames, 100 ms apart, in a window
examples/silx_live_view/run_live_view.sh build 8 500 --no-gui     # print each frame instead
```

Or run the two by hand, in the same directory:

```
build/examples/detector_pipeline/detector_writer 500 50 0 &
PYTHONPATH=build/python python3 examples/silx_live_view/silx_live_view.py detector_pipeline.h5
```

`--from-step 0` also delivers the steps committed before the viewer joined.
That is backfill (`H5Fsubscribe_from()`), which a live view usually doesn't
want. The CTest `python_silx_live_view` uses it so that the check "frames
0..7 arrived, 4 of them hits" doesn't depend on when the viewer started.

## Limits

- **No downsampling on the wire.** MAX IV's "downsample for the live view"
  would be a strided selection. The binding has none, and a stride would not
  cut bytes yet anyway: past 256 contiguous runs the writer sends the whole
  bounding span. A region of interest does work, as a box that follows
  growth: `subscribe({path: ((0, r0, c0), (None, nr, nc))}, tail=True)`.
- **Only the newest frame is drawn,** with no binning or averaging. Reduction
  selects values; it never computes them.
