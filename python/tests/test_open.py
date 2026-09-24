"""P1 exit gate (docs/python-plan.md): import, open and close, without the transport.

usage: test_open.py <stream_fixture executable>
"""

import gc
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

import volstream

FIXTURE = None


class OpenCloseTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.tmp.name, "stream.h5")
        env = dict(os.environ)
        env.pop("VOL_STREAM_NA", None)
        subprocess.run([FIXTURE, self.path], check=True, env=env)

    def tearDown(self):
        self.tmp.cleanup()

    def test_open_and_close(self):
        f = volstream.open(self.path)
        self.assertIsInstance(f, volstream.File)
        self.assertFalse(f.closed)
        self.assertEqual(f.path, self.path)
        self.assertIn("open", repr(f))
        f.close()
        self.assertTrue(f.closed)
        self.assertIn("closed", repr(f))

    def test_close_twice(self):
        f = volstream.open(self.path)
        f.close()
        f.close()
        self.assertTrue(f.closed)

    def test_pathlike(self):
        f = volstream.open(pathlib.Path(self.path))
        self.assertEqual(f.path, self.path)
        f.close()

    def test_keyword(self):
        volstream.open(path=self.path).close()

    def test_missing_file_raises(self):
        missing = os.path.join(self.tmp.name, "missing.h5")
        with self.assertRaises(volstream.Error) as ctx:
            volstream.open(missing)
        self.assertIn("missing.h5", str(ctx.exception))

    def test_error_is_an_exception(self):
        self.assertTrue(issubclass(volstream.Error, Exception))

    def test_bad_argument(self):
        with self.assertRaises(TypeError):
            volstream.open(42)

    def test_dropped_file_can_be_reopened(self):
        # A File dropped without close() is closed by its destructor, so the
        # same path opens again cleanly each time.
        for _ in range(50):
            volstream.open(self.path)
            gc.collect()
        volstream.open(self.path).close()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    FIXTURE = sys.argv.pop(1)
    unittest.main()
