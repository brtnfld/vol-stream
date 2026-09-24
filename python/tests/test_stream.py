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

usage: test_stream.py <stream_writer executable> <column|narrowing>
"""

import os
import subprocess
import sys
import tempfile
import time
import unittest

import numpy as np

import volstream

WRITER = None
ROWS, COLS, COL = 6, 8, 3
LOCKSTEP, LAST = 3, 7


def value(s, r, c):
    return s * 10000 + r * 100 + c


class StreamTest(unittest.TestCase):
    mode = None

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.sync = self.tmp.name
        self.path = os.path.join(self.tmp.name, "stream.h5")
        env = dict(os.environ)
        env.setdefault("VOL_STREAM_NA", "na+sm")
        self.writer = subprocess.Popen([WRITER, self.mode, self.path, self.sync], env=env)
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


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    WRITER = sys.argv.pop(1)
    mode = sys.argv.pop(1)
    cases = {"column": ColumnTest, "narrowing": NarrowingTest}
    if mode not in cases:
        sys.exit(__doc__)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(cases[mode])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
