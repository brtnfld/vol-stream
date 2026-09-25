#!/usr/bin/env python3
# * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
# Copyright by The HDF Group.  All rights reserved.
# This file is part of vol-stream.  See the LICENSE file at the root of the
# source distribution, or https://www.hdfgroup.org/licenses.
# * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
"""A control-room live view of a detector stream, drawn with silx.

The viewer MAX IV ships for every detector is a silx widget that polls the
stream-receiver over HTTP at 1 Hz for the latest frame, with a receiver-side
downsampling option (Bell et al., arXiv 2601.05901). This is the same view
fed by vol-stream instead: the writer pushes each committed frame, already
narrowed to --dtype for this subscriber only, and the viewer draws the newest
one and skips the rest. Nothing it does can slow the writer -- it never acks,
so the writer never counts it as behind (see H5Fset_stream_queue_policy()).

The frame stack is discovered, not configured: the first rank-3 numeric
dataset in the writer's schema, unless --path names one.

    silx_live_view.py detector_pipeline.h5            # the silx window
    silx_live_view.py detector_pipeline.h5 --no-gui   # print frames instead

--no-gui needs only NumPy; the window needs silx and a Qt binding.
"""

import argparse
import sys
import time

import numpy as np

import volstream


def pick_stack(schema, path):
    """The subscribed path: --path, or the first rank-3 numeric dataset."""
    if path is not None:
        if path not in schema:
            raise SystemExit(f"{path!r} is not in the stream's schema: {sorted(schema)}")
        return path
    for var in schema.values():
        if not var.is_attr and var.dtype is not None and var.shape is not None and len(var.shape) == 3:
            return var.path
    raise SystemExit(f"no rank-3 dataset to view in the stream's schema: {sorted(schema)}")


class LiveStack:
    """The newest frame of a growing [nP, i, j] stack, pushed by the writer."""

    def __init__(self, fname, path, dtype, from_step, timeout_s):
        self.file = volstream.open(fname)
        schema = self.file.schema(int(timeout_s * 1000))
        self.path = pick_stack(schema, path)
        var = schema[self.path]
        self.file.subscribe(self.path, from_step=from_step)
        if dtype is not None:
            self.file.subscribe_type(self.path, dtype)
        self.dtype = np.dtype(dtype) if dtype is not None else var.dtype
        self.frame_shape = var.shape[1:]
        self.skipped = 0
        print(f"viewer: {self.path} {var.dtype}{list(var.shape)} -> frames of "
              f"{list(self.frame_shape)} as {self.dtype}", flush=True)

    def frames(self, timeout_ms):
        """Every frame committed since the last call, oldest first: (index, frame)."""
        out = []
        step = self.file.next_step(timeout_ms)
        while step is not None:
            if self.path in step:
                a = step[self.path]
                # Rows are frames; the step wrote the last one. Earlier rows
                # are masked (not sent this step) -- only the new one is read.
                out.append((a.shape[0] - 1, np.ma.getdata(a)[-1]))
            step = self.file.next_step(0)
        return out

    def latest(self, timeout_ms=0):
        """The newest frame since the last call, or None. Older ones are dropped."""
        got = self.frames(timeout_ms)
        if not got:
            return None
        self.skipped += len(got) - 1
        return got[-1]

    @property
    def ended(self):
        return self.file.end_of_stream

    def close(self):
        self.file.close()


def run_text(stack, args):
    """--no-gui: print every frame, then check what arrived against --expect-*."""
    seen, hits = set(), 0
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline and not stack.ended:
        for index, frame in stack.frames(500):
            if frame.dtype != stack.dtype or frame.shape != stack.frame_shape:
                print(f"viewer: FAIL frame {index} is {frame.dtype}{list(frame.shape)}")
                return 1
            hit = int(frame.max()) >= args.hit_threshold
            hits += hit
            seen.add(index)
            print(f"viewer: frame {index:4d}  max {int(frame.max()):6d}  sum {int(frame.sum(dtype=np.int64)):9d}"
                  f"  {'HIT' if hit else '   '}  ({frame.nbytes // 1024} KiB as {frame.dtype})", flush=True)
        if args.expect_frames and len(seen) >= args.expect_frames:
            break

    print(f"viewer: {len(seen)} frame(s), {hits} over {args.hit_threshold}")
    if args.expect_frames and seen != set(range(args.expect_frames)):
        print(f"viewer: FAIL expected frames 0..{args.expect_frames - 1}, got {sorted(seen)}")
        return 1
    if args.expect_hits is not None and hits != args.expect_hits:
        print(f"viewer: FAIL expected {args.expect_hits} hit frame(s), got {hits}")
        return 1
    return 0


def run_gui(stack, args):
    """The silx window: redraw the newest frame on a timer, drop the backlog."""
    from silx.gui import qt
    from silx.gui.plot import ImageView

    app = qt.QApplication.instance() or qt.QApplication(sys.argv)
    view = ImageView()
    view.setWindowTitle(f"vol-stream live: {stack.path}")
    view.setColormap("viridis", normalization="log")
    view.show()

    first = [True]

    def refresh():
        got = stack.latest(0)
        if got is not None:
            index, frame = got
            view.setImage(frame, resetzoom=first[0])
            first[0] = False
            view.setGraphTitle(f"frame {index}  ({frame.dtype}; {stack.skipped} skipped)")
        elif stack.ended:
            timer.stop()
            view.setGraphTitle(view.getGraphTitle() + "  -- end of stream")

    timer = qt.QTimer()
    timer.timeout.connect(refresh)
    timer.start(int(1000 / args.fps))
    return app.exec()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("file", nargs="?", default="detector_pipeline.h5", help="the writer's file")
    p.add_argument("--path", help="the frame stack (default: first rank-3 dataset in the schema)")
    p.add_argument("--dtype", default="int16",
                   help="type the writer converts frames to for this viewer; 'native' for none (default int16)")
    p.add_argument("--fps", type=float, default=10.0, help="redraw rate of the window (default 10)")
    p.add_argument("--from-step", type=int, default=None,
                   help="also receive the steps committed before joining, from this one on")
    p.add_argument("--timeout", type=float, default=60.0, help="seconds to wait for the writer (default 60)")
    p.add_argument("--no-gui", action="store_true", help="print each frame instead of drawing it")
    p.add_argument("--expect-frames", type=int, default=0, help="--no-gui: fail unless frames 0..N-1 arrive")
    p.add_argument("--expect-hits", type=int, default=None, help="--no-gui: fail unless N frames are hits")
    p.add_argument("--hit-threshold", type=int, default=1000, help="a frame whose max reaches this is a hit")
    args = p.parse_args()

    dtype = None if args.dtype == "native" else args.dtype
    stack = LiveStack(args.file, args.path, dtype, args.from_step, args.timeout)
    try:
        return run_text(stack, args) if args.no_gui else run_gui(stack, args)
    finally:
        stack.close()


if __name__ == "__main__":
    sys.exit(main())
