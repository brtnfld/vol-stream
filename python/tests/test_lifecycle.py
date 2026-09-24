"""P3 exit gate (docs/python-plan.md): a Python consumer that behaves under
early exit, Ctrl-C, other threads, and fork().

Follows stream_writer.c over na+sm. The same file is both the driver (one
ctest entry per mode) and the consumer process each driver starts, so the
consumer's exit and signals are real process events.

  break      The consumer reads three steps, breaks out, and exits without
             close(). It must exit promptly, and the writer's later steps
             must not pay the push timeout a reader that never left costs.
  interrupt  While the consumer waits in next_step(), another Python thread
             must keep running, and SIGINT must raise KeyboardInterrupt within
             a second rather than when the wait ends.
  fork       A forked child gets a clear error from the parent's File and
             from open(), and closing or dropping the File there does not
             hang. The parent keeps working.

usage: test_lifecycle.py <stream_writer executable> <break|interrupt|fork>
       test_lifecycle.py consumer-<mode> <file> <syncdir>     (internal)
"""

import os
import signal
import subprocess
import sys
import tempfile
import threading
import time

ROWS, COLS = 6, 8
MAX_COMMIT_MS = 500  # a push timeout to a departed reader is 1000 ms


def touch(sync, name):
    open(os.path.join(sync, name), "w").close()


def wait_for(sync, name, seconds=60, proc=None):
    deadline = time.monotonic() + seconds
    target = os.path.join(sync, name)
    while not os.path.exists(target):
        if proc is not None and proc.poll() is not None:
            raise AssertionError(f"process exited with {proc.returncode} before {name!r}")
        if time.monotonic() > deadline:
            raise AssertionError(f"timed out waiting for {name!r}")
        time.sleep(0.05)


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)
    print(f"  ok    {msg}", flush=True)


# ------------------------------------------------------------------ consumers

def consumer_break(path, sync):
    import volstream

    f = volstream.open(path)
    f.subscribe("/grid")  # the writer waits for this in H5Fwait_subscribers()
    n = 0
    for _ in f.steps(timeout=30):
        n += 1
        if n == 3:
            break
    if n != 3:
        sys.exit("consumer: a step never arrived")
    # Deliberately no close(): the atexit backstop must leave the group.
    globals()["kept_open"] = f
    sys.exit(0)


def consumer_interrupt(path, sync):
    import volstream

    # CPython installs its KeyboardInterrupt handler only if SIGINT was not
    # already ignored when it started, and a process launched from a
    # non-interactive CI shell can inherit it ignored. This test is about
    # the binding, not that environment, so install the handler explicitly.
    print(f"  info  inherited SIGINT handler: {signal.getsignal(signal.SIGINT)}", flush=True)
    signal.signal(signal.SIGINT, signal.default_int_handler)

    f = volstream.open(path)
    f.subscribe("/grid")
    touch(sync, "ready")

    count = [0]
    stop = threading.Event()

    def spin():
        while not stop.is_set():
            count[0] += 1
            time.sleep(0.001)

    t = threading.Thread(target=spin)
    t.start()
    step = f.next_step(1000)  # the writer is idle, so this times out
    stop.set()
    t.join()
    check(step is None, "next_step() timed out with the writer idle")
    check(count[0] > 100, f"another thread ran {count[0]} times during a 1 s wait (GIL released)")

    touch(sync, "blocked")
    t0 = time.monotonic()
    try:
        f.next_step(60000)
    except KeyboardInterrupt:
        waited = time.monotonic() - t0
        f.close()
        print(f"  info  interrupted after {waited:.2f} s", flush=True)
        sys.exit(0)
    sys.exit("consumer: next_step() returned instead of raising KeyboardInterrupt")


def consumer_fork(path, sync):
    import numpy as np

    import volstream

    f = volstream.open(path)
    f.subscribe("/grid")
    touch(sync, "ready")

    pid = os.fork()
    if pid == 0:
        status = 0
        try:
            f.next_step(0)
            print("child: FAIL the parent's File worked after fork()", flush=True)
            status = 3
        except volstream.Error as e:
            if "fork" not in str(e):
                print(f"child: FAIL unclear error: {e}", flush=True)
                status = 3
        try:
            volstream.open(path)
            print("child: FAIL open() worked after fork()", flush=True)
            status = 3
        except volstream.Error as e:
            if "fork" not in str(e):
                print(f"child: FAIL unclear error from open(): {e}", flush=True)
                status = 3
        f.close()
        del f
        os._exit(status)

    deadline = time.monotonic() + 20
    while True:
        done, status = os.waitpid(pid, os.WNOHANG)
        if done:
            break
        if time.monotonic() > deadline:
            os.kill(pid, signal.SIGKILL)
            sys.exit("consumer: forked child hung")
        time.sleep(0.05)
    check(os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0,
          "child got a fork() error from File and open(), and closed without hanging")

    touch(sync, "go")
    step = f.next_step(10000)
    check(step is not None and "/grid" in step, "parent still receives steps after the fork")
    expected = np.array([[10000 + r * 100 + c for c in range(COLS)] for r in range(ROWS)])
    np.testing.assert_array_equal(step["/grid"], expected)
    f.close()
    sys.exit(0)


CONSUMERS = {"break": consumer_break, "interrupt": consumer_interrupt, "fork": consumer_fork}


# ------------------------------------------------------------------ drivers

def run(writer_exe, mode):
    tmp = tempfile.TemporaryDirectory()
    sync = tmp.name
    path = os.path.join(sync, "stream.h5")
    env = dict(os.environ)
    env.setdefault("VOL_STREAM_NA", "na+sm")
    writer_mode = {"break": "lifecycle", "interrupt": "idle", "fork": "idle"}[mode]
    writer_env = dict(env, STREAM_WRITER_SUBSCRIBERS="1") if mode == "break" else env
    writer = subprocess.Popen([writer_exe, writer_mode, path, sync], env=writer_env, stdout=subprocess.PIPE, text=True)
    consumer = None
    try:
        wait_for(sync, "committed", proc=writer)
        consumer = subprocess.Popen([sys.executable, __file__, f"consumer-{mode}", path, sync], env=env)

        if mode == "break":
            t0 = time.monotonic()
            rc = consumer.wait(timeout=30)
            check(rc == 0, f"consumer broke out early and exited without close() in {time.monotonic() - t0:.1f} s")
            touch(sync, "gone")
            touch(sync, "done")  # the writer waits for it after its last step
            out, _ = writer.communicate(timeout=60)
            check(writer.returncode == 0, "writer finished every step after the consumer left")
            worst = float(out.split("max_commit_ms")[1].split()[0])
            check(worst < MAX_COMMIT_MS,
                  f"slowest commit after the consumer left was {worst:.1f} ms (< {MAX_COMMIT_MS} ms: it left cleanly)")

        elif mode == "interrupt":
            wait_for(sync, "blocked", proc=consumer)
            time.sleep(0.5)
            t0 = time.monotonic()
            consumer.send_signal(signal.SIGINT)
            rc = consumer.wait(timeout=10)
            took = time.monotonic() - t0
            check(rc == 0, "consumer raised KeyboardInterrupt from inside next_step()")
            check(took < 2.0, f"it exited {took:.2f} s after SIGINT, not at the end of its 60 s wait")

        else:
            rc = consumer.wait(timeout=60)
            check(rc == 0, "fork scenario passed in the consumer")

        touch(sync, "done")
        if writer.poll() is None:
            writer.communicate(timeout=60)
        check(writer.returncode == 0, "writer exited cleanly")
        return 0
    except (AssertionError, subprocess.TimeoutExpired) as e:
        print(f"  FAIL  {e}", flush=True)
        return 1
    finally:
        for p in (consumer, writer):
            if p is not None and p.poll() is None:
                p.kill()
                p.wait()
        tmp.cleanup()


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1].startswith("consumer-"):
        CONSUMERS[sys.argv[1][len("consumer-"):]](sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 3 and sys.argv[2] in CONSUMERS:
        sys.exit(run(sys.argv[1], sys.argv[2]))
    else:
        sys.exit(__doc__)
