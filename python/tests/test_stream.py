"""P2 exit gate (docs/python-plan.md): whole steps from a live C writer, as NumPy arrays.

Follows stream_writer.c over na+sm. Each mode runs in its own process (one
ctest entry per mode) so every scenario gets a fresh transport.

  column     A one-column selection of a row-major grid, so every step arrives
             as one push per row. Steps are checked one at a time while the
             writer waits after each, then after the writer has run several
             steps ahead. The reader joins after step 0 is committed, so that
             step must never be returned.
  narrowing  The whole grid, delivered as int16 and filtered by a predicate:
             one step where everything matches, one where some rows do, and
             one where nothing does.
  iterate    follow() and `for step in f.steps(max_steps=3)`: the
             schema-driven path, run to completion.
  getonly    Only get(), for every push of every step. Afterwards no step
             notification may be left queued: get() drains them.
  torch      StreamDataset through a DataLoader. Skipped (exit 77) if torch
             is not installed.
  eos        The writer commits three steps and closes. Unbounded iteration
             must yield all three and then end by itself.
  ack        backpressure=True against a writer with the Block queue policy:
             a consumer that takes 0.4 s per step must slow the writer down.
  noack      The same without backpressure: the writer must not wait, which
             is what a subscriber that never acks gets.
  twostreams Two different live streams open at once in one process.
  samestream The same stream opened twice at once in one process; both
             copies must receive every step.
  reopen     Open, close, and open the same stream again in one process.
  drop       The column scenario with one push lost on the way (the writer's
             test-only VOL_STREAM_TEST_DROP_PUSH): that step must arrive as a
             masked array with exactly the lost row masked.

usage: test_stream.py <stream_writer executable> <column|narrowing|iterate|getonly|torch|eos|drop|ack|noack|twostreams|samestream|reopen>
"""

import os
import subprocess
import sys
import tempfile
import time
import unittest

import numpy as np

# The reader in this process needs the transport as much as the writer does;
# default it here so the script works however it is launched, not only
# under ctest (which sets it).
os.environ.setdefault("VOL_STREAM_NA", "na+sm")

import volstream  # noqa: E402

WRITER = None
ROWS, COLS, COL = 6, 8, 3
LOCKSTEP, LAST = 3, 7


def value(s, r, c):
    return s * 10000 + r * 100 + c


class StreamTest(unittest.TestCase):
    mode = None
    writer_env = {}
    capture_writer = False

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.sync = self.tmp.name
        self.path = os.path.join(self.tmp.name, "stream.h5")
        env = dict(os.environ)
        env.setdefault("VOL_STREAM_NA", "na+sm")
        env.update(self.writer_env)
        self.writer = subprocess.Popen([WRITER, self.mode, self.path, self.sync], env=env,
                                       stdout=subprocess.PIPE if self.capture_writer else None, text=True)
        self.file = None

    def tearDown(self):
        if self.file is not None:
            self.file.close()
        self.touch("done")
        try:
            self.writer.wait(timeout=60)
        except subprocess.TimeoutExpired:
            self.writer.kill()
            self.writer.wait()
        self.tmp.cleanup()

    def touch(self, name):
        open(os.path.join(self.sync, name), "w").close()

    def wait_for(self, name, seconds=60):
        deadline = time.monotonic() + seconds
        target = os.path.join(self.sync, name)
        while not os.path.exists(target):
            if self.writer.poll() is not None:
                self.fail(f"writer exited with {self.writer.returncode} before {name!r}")
            if time.monotonic() > deadline:
                self.fail(f"timed out waiting for {name!r}")
            time.sleep(0.05)

    def attach(self):
        self.wait_for("committed")
        self.file = volstream.open(self.path)

    def assert_writer_ok(self):
        self.touch("done")
        self.file.close()
        self.assertEqual(self.writer.wait(timeout=60), 0, "writer reported failure")


class ColumnTest(StreamTest):
    mode = "column"

    def check_column(self, step, s):
        self.assertIsNotNone(step, f"step {s} never arrived")
        self.assertIn("/grid", step, f"step {s} carried nothing for /grid")
        a = step["/grid"]
        self.assertNotIsInstance(a, np.ma.MaskedArray, f"step {s} arrived incomplete")
        self.assertEqual(a.shape, (ROWS, 1))
        self.assertEqual(a.dtype, np.dtype("int32"))
        expected = np.array([[value(s, r, COL)] for r in range(ROWS)], dtype=np.int32)
        np.testing.assert_array_equal(a, expected, err_msg=f"step {s}")

    def test_column(self):
        self.attach()
        schema = self.file.schema()
        self.assertEqual(schema["/grid"].shape, (ROWS, COLS))
        self.assertEqual(schema["/grid"].dtype, np.dtype("int32"))

        self.file.subscribe({"/grid": ((0, COL), (ROWS, 1))})
        self.touch("ready")

        phys = []
        # Lockstep: the writer waits for us after each step. Step 0 was
        # committed before we subscribed, so the first step must be 1.
        for s in range(1, LOCKSTEP + 1):
            step = self.file.next_step(20000)
            self.check_column(step, s)
            phys.append(step.phys)
            self.touch(f"ack.{s}")

        # Lagging: the writer has committed the rest before we look.
        self.wait_for("writes_done")
        for s in range(LOCKSTEP + 1, LAST + 1):
            step = self.file.next_step(5000)
            self.check_column(step, s)
            phys.append(step.phys)

        self.assertEqual(phys, sorted(set(phys)), "physical steps not strictly increasing")
        self.assertIsNone(self.file.next_step(500), "a step arrived after the last one")
        self.assert_writer_ok()


class NarrowingTest(StreamTest):
    mode = "narrowing"

    def test_type_and_predicate(self):
        self.attach()
        self.file.subscribe("/grid")
        self.file.subscribe_type("/grid", np.int16)
        self.file.subscribe_predicate("/grid", "<", 20300)
        self.touch("ready")
        self.wait_for("writes_done")

        full = np.array([[value(1, r, c) for c in range(COLS)] for r in range(ROWS)])

        # Step 1: 10000..10507, everything matches.
        step = self.file.next_step(5000)
        self.assertIsNotNone(step)
        a = step["/grid"]
        self.assertNotIsInstance(a, np.ma.MaskedArray)
        self.assertEqual(a.dtype, np.dtype("int16"))
        np.testing.assert_array_equal(a, full)

        # Step 2: rows 0-2 (20000..20207) match, rows 3-5 do not.
        step = self.file.next_step(5000)
        self.assertIsNotNone(step)
        a = step["/grid"]
        self.assertIsInstance(a, np.ma.MaskedArray)
        self.assertEqual(a.dtype, np.dtype("int16"))
        self.assertFalse(a.mask[:3].any(), "matching rows were masked")
        self.assertTrue(a.mask[3:].all(), "non-matching rows were not masked")
        np.testing.assert_array_equal(a.data[:3], full[:3] + 10000)

        # Step 3: 30000..30507, nothing matches, so nothing is sent.
        step = self.file.next_step(5000)
        self.assertIsNotNone(step, "a step with no matching data must still be reported")
        self.assertNotIn("/grid", step)

        self.assertIsNone(self.file.next_step(500))
        self.assert_writer_ok()


class DropTest(StreamTest):
    mode = "column"
    # Step 0 is committed before the reader subscribes, so it sends nothing;
    # steps 1 and 2 send one push per row. Push 9 is step 2, row 3.
    DROPPED_STEP, DROPPED_ROW = 2, 3
    writer_env = {"VOL_STREAM_TEST_DROP_PUSH": str(ROWS * (DROPPED_STEP - 1) + DROPPED_ROW)}

    def test_lost_push_is_masked(self):
        self.attach()
        self.file.subscribe({"/grid": ((0, COL), (ROWS, 1))})
        self.touch("ready")
        for s in range(1, LAST + 1):
            if s > LOCKSTEP:
                self.wait_for("writes_done")
            step = self.file.next_step(20000)
            self.assertIsNotNone(step, f"step {s} never arrived")
            a = step["/grid"]
            expected = np.array([[value(s, r, COL)] for r in range(ROWS)], dtype=np.int32)
            if s == self.DROPPED_STEP:
                self.assertIsInstance(a, np.ma.MaskedArray, "a step with a lost push came back as complete")
                self.assertEqual(a.mask[:, 0].tolist(), [r == self.DROPPED_ROW for r in range(ROWS)])
                keep = [r for r in range(ROWS) if r != self.DROPPED_ROW]
                np.testing.assert_array_equal(a.data[keep], expected[keep])
            else:
                self.assertNotIsInstance(a, np.ma.MaskedArray, f"step {s} arrived incomplete")
                np.testing.assert_array_equal(a, expected, err_msg=f"step {s}")
            if s <= LOCKSTEP:
                self.touch(f"ack.{s}")
        self.assert_writer_ok()


class BackpressureTest(StreamTest):
    mode = "block"
    capture_writer = True
    backpressure = True
    PER_STEP_S = 0.4

    def run_consumer(self):
        self.wait_for("committed")
        with volstream.follow(self.path, "/grid", backpressure=self.backpressure) as f:
            self.touch("ready")
            for s, step in enumerate(f.steps(max_steps=6, timeout=60), start=1):
                np.testing.assert_array_equal(step["/grid"], whole_grid(s))
                time.sleep(self.PER_STEP_S)
            self.assertEqual(s, 6, "not every step arrived")
            self.touch("done")
            out, _ = self.writer.communicate(timeout=60)
        self.assertEqual(self.writer.returncode, 0)
        return float(out.split("writer_ms")[1].split()[0])

    def test_backpressure(self):
        writer_ms = self.run_consumer()
        # Steps 3..6 each wait for the consumer's ack of the step before last,
        # about PER_STEP_S apiece; unthrottled, six steps take milliseconds.
        self.assertGreater(writer_ms, 1000, f"the writer did not wait for an acking reader ({writer_ms:.0f} ms)")


class NoBackpressureTest(BackpressureTest):
    backpressure = False

    def test_backpressure(self):
        writer_ms = self.run_consumer()
        self.assertLess(writer_ms, 1000, f"the writer waited for a reader that never acks ({writer_ms:.0f} ms)")


class MultiFileTest(StreamTest):
    """One process, more than one File. The writer is in narrowing mode:
    step 0 before "ready", then steps 1..3 back to back."""

    mode = "narrowing"

    def read_three(self, f, who):
        got = [step["/grid"] for step in f.steps(max_steps=3, timeout=30)]
        self.assertEqual(len(got), 3, f"{who}: not every step arrived")
        for s, a in enumerate(got, start=1):
            np.testing.assert_array_equal(a, whole_grid(s), err_msg=f"{who}, step {s}")

    def second_writer(self):
        sync = os.path.join(self.tmp.name, "second")
        os.mkdir(sync)
        path = os.path.join(sync, "stream.h5")
        writer = subprocess.Popen([WRITER, "narrowing", path, sync], env=dict(os.environ))
        return writer, path, sync

    def test_twostreams(self):
        writer2, path2, sync2 = self.second_writer()
        try:
            self.wait_for("committed")
            deadline = time.monotonic() + 60
            while not os.path.exists(os.path.join(sync2, "committed")):
                self.assertLess(time.monotonic(), deadline, "second writer never committed")
                time.sleep(0.05)
            with volstream.follow(self.path, "/grid") as a, volstream.follow(path2, "/grid") as b:
                self.touch("ready")
                open(os.path.join(sync2, "ready"), "w").close()
                self.read_three(a, "first stream")
                self.read_three(b, "second stream")
        finally:
            open(os.path.join(sync2, "done"), "w").close()
            self.assertEqual(writer2.wait(timeout=60), 0, "second writer failed")
        self.touch("done")
        self.assertEqual(self.writer.wait(timeout=60), 0)

    def test_samestream(self):
        self.wait_for("committed")
        with volstream.follow(self.path, "/grid") as a, volstream.follow(self.path, "/grid") as b:
            self.touch("ready")
            self.read_three(a, "first File")
            self.read_three(b, "second File")
        self.touch("done")
        self.assertEqual(self.writer.wait(timeout=60), 0)

    def test_reopen(self):
        self.wait_for("committed")
        with volstream.follow(self.path, "/grid") as a:
            self.assertFalse(a.closed)
        with volstream.follow(self.path, "/grid") as b:
            self.touch("ready")
            self.read_three(b, "reopened File")
        self.touch("done")
        self.assertEqual(self.writer.wait(timeout=60), 0)


def whole_grid(s):
    return np.array([[value(s, r, c) for c in range(COLS)] for r in range(ROWS)], dtype=np.int32)


class IterateTest(StreamTest):
    mode = "narrowing"  # the writer commits steps 1..3 back to back

    def test_follow_and_iterate(self):
        self.wait_for("committed")
        with volstream.follow(self.path) as f:
            self.touch("ready")
            seen = []
            for step in f.steps(max_steps=3, timeout=30):
                seen.append(step)
            self.assertEqual(len(seen), 3, "iteration ended before max_steps")
            for s, step in enumerate(seen, start=1):
                np.testing.assert_array_equal(step["/grid"], whole_grid(s), err_msg=f"step {s}")
            # A quiet stream ends iteration only on a bound the caller gave.
            t0 = time.monotonic()
            self.assertEqual(list(f.steps(idle_timeout=0.5)), [])
            self.assertLess(time.monotonic() - t0, 3)
        self.assertTrue(f.closed, "leaving the with block closed the file")
        self.touch("done")
        self.assertEqual(self.writer.wait(timeout=60), 0)


class GetOnlyTest(StreamTest):
    mode = "narrowing"

    def test_get_only_leaves_no_notifications(self):
        self.attach()
        self.file.subscribe("/grid")
        self.touch("ready")
        self.wait_for("writes_done")
        pushes = []
        deadline = time.monotonic() + 30
        while len(pushes) < 3 and time.monotonic() < deadline:
            p = self.file.get(1000)
            if p is not None:
                pushes.append(p)
        self.assertEqual([p.phys for p in pushes], sorted(p.phys for p in pushes))
        self.assertEqual(len(pushes), 3, "a whole-grid subscription is one push per step")
        for s, p in enumerate(pushes, start=1):
            np.testing.assert_array_equal(p.data.reshape(ROWS, COLS), whole_grid(s))
        self.assertIsNone(self.file.get(200))
        self.assertIsNone(self.file._raw.wait_step_ready(0), "step notifications were left to accumulate")
        self.assert_writer_ok()


class TorchTest(StreamTest):
    mode = "narrowing"

    def test_dataloader(self):
        import torch
        from volstream.torch import StreamDataset

        self.wait_for("committed")
        ds = StreamDataset(self.path, "/grid", max_steps=3, timeout=30)
        self.touch("ready")
        batches = list(torch.utils.data.DataLoader(ds, batch_size=3, num_workers=0))
        self.assertEqual(len(batches), 1)
        self.assertEqual(tuple(batches[0].shape), (3, ROWS, COLS))
        expected = np.stack([whole_grid(s) for s in (1, 2, 3)])
        np.testing.assert_array_equal(batches[0].numpy(), expected)
        with self.assertRaises(TypeError):
            import pickle

            pickle.dumps(ds)
        ds.close()
        self.touch("done")
        self.assertEqual(self.writer.wait(timeout=60), 0)


class EndOfStreamTest(StreamTest):
    mode = "eos"

    def test_iteration_ends_when_writer_leaves(self):
        self.wait_for("committed")
        with volstream.follow(self.path) as f:
            self.assertFalse(f.end_of_stream, "end of stream reported with the writer still in the group")
            self.touch("ready")
            t0 = time.monotonic()
            # The timeout is only a backstop so a broken build fails rather
            # than hangs; ending well before it is the assertion.
            seen = list(f.steps(timeout=60))
            took = time.monotonic() - t0
            self.assertEqual([s["/grid"][0, 0] for s in seen], [value(s, 0, 0) for s in (1, 2, 3)])
            self.assertLess(took, 30, f"iteration ended only at its timeout ({took:.1f} s)")
            self.assertTrue(f.end_of_stream)
            self.assertIsNone(f.next_step(10000))
        self.assertEqual(self.writer.wait(timeout=60), 0)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    WRITER = sys.argv.pop(1)
    mode = sys.argv.pop(1)
    cases = {"column": ColumnTest, "narrowing": NarrowingTest, "iterate": IterateTest,
             "getonly": GetOnlyTest, "torch": TorchTest, "eos": EndOfStreamTest, "drop": DropTest,
             "ack": BackpressureTest, "noack": NoBackpressureTest,
             "twostreams": MultiFileTest, "samestream": MultiFileTest, "reopen": MultiFileTest}
    if mode not in cases:
        sys.exit(__doc__)
    if mode == "torch":
        try:
            import torch  # noqa: F401
        except ImportError:
            print("torch is not installed; skipping")
            sys.exit(77)
    if cases[mode] is MultiFileTest:
        suite = unittest.TestSuite([MultiFileTest(f"test_{mode}")])
    else:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(cases[mode])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
