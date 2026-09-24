"""The File API over the raw extension, and per-step reassembly."""

import atexit
import json
import math
import time
import weakref
from typing import NamedTuple, Optional

import numpy as np

from . import _volstream
from ._volstream import Error

_OPS = {
    "<": _volstream.PRED_LT,
    "<=": _volstream.PRED_LE,
    ">": _volstream.PRED_GT,
    ">=": _volstream.PRED_GE,
    "==": _volstream.PRED_EQ,
    "!=": _volstream.PRED_NE,
}


class Var(NamedTuple):
    """One object the stream carries, as the writer describes it."""

    path: str
    dtype: Optional[np.dtype]  # None for variable-length, reference and similar types
    shape: tuple
    is_attr: bool


class Push(NamedTuple):
    """One payload exactly as the writer sent it."""

    phys: int
    path: str
    start: int  # flat element index of data[0] in the whole object
    data: np.ndarray  # 1-D


class Step:
    """One committed step: a physical step number and one array per path.

    A path appears only if at least one push for it arrived in this step. Its
    array has the shape of the subscribed selection. If every selected element
    arrived it is a plain ndarray; otherwise it is a numpy.ma.MaskedArray whose
    mask marks the elements that did not arrive. That happens with a predicate
    subscription, with a writer that wrote only part of the object this step,
    and if a push was lost.
    """

    __slots__ = ("phys", "wall_time_ns", "arrays")

    def __init__(self, phys, wall_time_ns, arrays):
        self.phys = phys
        self.wall_time_ns = wall_time_ns
        self.arrays = arrays

    def __getitem__(self, path):
        return self.arrays[path]

    def __contains__(self, path):
        return path in self.arrays

    def __repr__(self):
        return f"<volstream.Step phys={self.phys} paths={sorted(self.arrays)}>"


def _dtype(desc):
    """A NumPy dtype for the extension's JSON type description, or None."""
    k = desc.get("k")
    if k in ("i", "u", "f"):
        return np.dtype(f"{desc['o']}{k}{desc['s']}")
    if k == "S":
        return np.dtype(f"S{desc['s']}")
    if k == "V":
        return np.dtype(f"V{desc['s']}")
    if k == "array":
        base = _dtype(desc["base"])
        return None if base is None else np.dtype((base, tuple(desc["dims"])))
    if k == "compound":
        names, formats, offsets = [], [], []
        for m in desc["members"]:
            t = _dtype(m["type"])
            if t is None:
                return None
            names.append(m["name"])
            formats.append(t)
            offsets.append(m["offset"])
        return np.dtype({"names": names, "formats": formats, "offsets": offsets, "itemsize": desc["s"]})
    return None


class _Subscription:
    def __init__(self, var, start, count, whole=False):
        self.path = var.path
        # A whole-dataset subscription follows growth along the first
        # dimension: the returned array covers whatever rows arrived.
        self.whole = whole
        self.dims = var.shape
        self.dtype = var.dtype
        self.native_dtype = var.dtype  # what the writer sends without narrowing
        self.start = tuple(start)
        self.count = tuple(count)
        self.strides = tuple(math.prod(self.dims[k + 1 :]) for k in range(len(self.dims)))
        self.nelem = math.prod(self.count)
        first = sum(s * st for s, st in zip(self.start, self.strides))
        last = sum((s + c - 1) * st for s, c, st in zip(self.start, self.count, self.strides))
        self.flat_first = first
        self.contiguous = self.nelem > 0 and last - first + 1 == self.nelem

    def values(self, push_bytes, elem_count):
        """The push as a 1-D array in the subscribed dtype."""
        size = len(push_bytes)
        if size == elem_count * self.dtype.itemsize:
            return np.frombuffer(push_bytes, dtype=self.dtype, count=elem_count)
        # The writer falls back to the object's own type when it cannot
        # convert (see H5Fsubscribe_type()); honour the request here instead.
        if self.native_dtype is not None and size == elem_count * self.native_dtype.itemsize:
            return np.frombuffer(push_bytes, dtype=self.native_dtype, count=elem_count).astype(self.dtype)
        raise Error(
            f"push for {self.path!r} is {size} bytes for {elem_count} elements, which matches "
            f"neither {self.dtype} nor {self.native_dtype}"
        )

    def assemble(self, pushes):
        if not self.dims:  # scalar
            return pushes[-1].data.reshape(())

        if (
            len(pushes) == 1
            and self.contiguous
            and pushes[0].start == self.flat_first
            and len(pushes[0].data) == self.nelem
        ):
            return pushes[0].data.reshape(self.count)

        count = self.count
        if self.whole:
            # Rows past the extent seen at subscribe time: the dataset grew.
            last = max(p.start + len(p.data) for p in pushes) - 1
            count = (max(count[0], last // self.strides[0] + 1),) + count[1:]

        out = np.zeros(count, dtype=self.dtype)
        got = np.zeros(count, dtype=bool)
        for p in pushes:
            flat = np.arange(p.start, p.start + len(p.data), dtype=np.int64)
            keep = np.ones(len(flat), dtype=bool)
            local = []
            for k, (st, dim, s0, c) in enumerate(zip(self.strides, self.dims, self.start, count)):
                coord = flat // st if k == 0 else (flat // st) % dim
                coord = coord - s0
                keep &= (coord >= 0) & (coord < c)
                local.append(coord)
            index = tuple(c[keep] for c in local)
            out[index] = p.data[keep]
            got[index] = True
        if got.all():
            return out
        return np.ma.MaskedArray(out, mask=~got)


# Every File still open, so they can be closed before the interpreter shuts
# down. A reader that is closed leaves the writer's group at once; one that is
# not costs the writer a push timeout on every step until the transport
# notices it is gone.
_open_files = weakref.WeakSet()


@atexit.register
def _close_all():
    for f in list(_open_files):
        try:
            f.close()
        except Exception:
            pass


def _growing_extent(shape):
    """The extent a whole-dataset subscription is made against: the dataset's
    own, with dimension 0 made as large as it can be.

    The writer only sends elements inside a subscription's selection, and
    dimension 0 of a subscription may differ from the dataset's (its routing
    requires only the trailing dimensions to match). So subscribing against a
    huge first dimension is what makes rows written after subscribe() --
    a growing dataset -- arrive at all. Kept under 2**62 elements so the
    writer's flat element arithmetic cannot overflow.
    """
    if not shape:
        return shape
    trailing = math.prod(shape[1:]) or 1
    return (max(shape[0], 2**62 // trailing),) + tuple(shape[1:])


class File:
    """A vol-stream file opened for reading. Create with volstream.open().

    Close it with close() or by using it as a context manager. A File left
    open is closed when the interpreter exits.

    With backpressure=True, next_step() tells the writer each step it hands
    back has been consumed, so a writer with a queue policy (see
    H5Fset_stream_queue_policy() in the C API) counts this reader, and under
    Block waits for it. Off by default: under Block, a consumer that stalls
    then stalls the writer.
    """

    def __init__(self, raw, backpressure=False):
        self._raw = raw
        self.backpressure = backpressure
        self._subs = {}
        self._held = None  # a Push popped past the end of a step, for a later one
        _open_files.add(self)

    @property
    def path(self):
        return self._raw.path

    @property
    def closed(self):
        return self._raw.closed

    def close(self):
        """Close the file. Safe to call more than once, and in a forked child."""
        self._held = None
        _open_files.discard(self)
        self._raw.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __repr__(self):
        return f"<volstream.File {self.path!r} ({'closed' if self.closed else 'open'})>"

    def schema(self, timeout_ms=10000):
        """Ask the writer what the stream carries. Returns {path: Var}.

        Waits up to timeout_ms for the writer's first committed step, since a
        writer that has not committed one has nothing to describe yet.
        """
        _, entries = self._raw.schema(timeout_ms)
        return {
            path: Var(path, _dtype(json.loads(type_json)), tuple(dims) if dims is not None else None, is_attr)
            for path, is_attr, dims, type_json in entries
        }

    def subscribe(self, selections, timeout_ms=10000, deflate=None):
        """Subscribe to one or more datasets.

        selections is a path, a list of paths, or a dict mapping each path to
        None (the whole dataset) or a (start, count) pair of tuples. Shapes
        and types come from the writer's schema.

        The first subscribe() on a file also discards every step committed
        before it: those steps carry nothing for this reader, including the
        one a late join announces for the writer's current step. Later calls
        add or replace subscriptions without discarding anything. Subscribing
        to a path again clears any subscribe_type() or subscribe_predicate()
        on it.

        deflate=level (0-9) has the writer compress these paths' data in
        transit, for this subscriber only; what next_step() returns is always
        decoded. Use separate subscribe() calls for different settings.
        """
        if isinstance(selections, str):
            selections = {selections: None}
        elif not isinstance(selections, dict):
            selections = {path: None for path in selections}

        schema = self.schema(timeout_ms)
        entries, subs = [], {}
        for path, sel in selections.items():
            var = schema.get(path)
            if var is None:
                raise KeyError(f"{path!r} is not in the stream's schema")
            if var.dtype is None:
                raise NotImplementedError(
                    f"{path!r} has a type this binding cannot deliver (variable-length, reference, "
                    "or bitfield)")
            if var.is_attr and deflate is not None:
                raise ValueError(f"{path!r} is an attribute; attributes cannot be delivered deflated")
            if var.shape is None:
                raise NotImplementedError(f"{path!r} does not have a simple dataspace")
            if sel is None:
                start, count = (0,) * len(var.shape), var.shape
                extent = var.shape if var.is_attr else _growing_extent(var.shape)
                entry = (path, extent, None, None)
                chunk = tuple(max(1, d) for d in var.shape)
            else:
                start, count = (tuple(int(x) for x in s) for s in sel)
                entry = (path, var.shape, start, count)
                chunk = None
            entries.append(entry if deflate is None else entry + (int(deflate), chunk))
            subs[path] = _Subscription(var, start, count, whole=sel is None and not var.is_attr)

        # The backlog is drained before subscribing, not after: a writer
        # released by H5Fwait_subscribers() can announce its next step as
        # soon as the subscription reaches it, before this call returns.
        last = self._discard_backlog() if not self._subs else None
        self._raw.subscribe(entries)
        self._subs.update(subs)
        if last is not None and self.backpressure:
            # Those steps are done with as far as this reader is concerned;
            # acking them makes it a tracked reader from subscribe() on,
            # rather than only after its first step.
            self._raw.ack(last)

    def subscribe_type(self, path, dtype):
        """Have the writer convert path's data to dtype before sending it.

        dtype must be a native-byte-order integer or float type, or None to
        restore the dataset's own type.
        """
        sub = self._sub(path)
        if dtype is None:
            self._raw.subscribe_type(path, None, 0)
            sub.dtype = sub.native_dtype
            return
        dtype = np.dtype(dtype)
        if dtype.kind not in "iuf" or not dtype.isnative:
            raise ValueError(f"{dtype} is not a native-byte-order integer or float type")
        self._raw.subscribe_type(path, dtype.kind, dtype.itemsize)
        sub.dtype = dtype

    def subscribe_predicate(self, path, op, value):
        """Have the writer send only elements of path for which `element op value` holds.

        op is one of '<', '<=', '>', '>=', '==', '!='. A step where nothing
        matches sends nothing for path; one where some elements match arrives
        as a masked array.
        """
        self._sub(path)
        if op not in _OPS:
            raise ValueError(f"op must be one of {sorted(_OPS)}, not {op!r}")
        if isinstance(value, np.generic):
            value = value.item()
        self._raw.subscribe_predicate(path, _OPS[op], value)

    def get(self, timeout_ms=0):
        """Return the next raw Push, or None if none arrives within timeout_ms.

        This is one payload, not one step: a step can arrive as several
        pushes. Use next_step() or iterate the File for whole steps.

        get() also discards any step notifications already queued, which a
        get()-only consumer has no use for; left alone they would accumulate
        for as long as it runs. That is also why get() and next_step() must
        not be mixed on one file: next_step() needs those notifications.
        """
        item = self._raw.get(timeout_ms)
        while self._raw.wait_step_ready(0) is not None:
            pass
        if item is None:
            return None
        return self._push(item)

    def next_step(self, timeout_ms=10000):
        """Wait up to timeout_ms for the next committed step and return it as a Step.

        Returns None if no step is committed in time.
        """
        ready = self._raw.wait_step_ready(timeout_ms)
        if ready is None:
            return None
        phys, wall_ns = ready

        # The writer delivers every push of a step before announcing it, so
        # everything for this step is already queued: drain without waiting.
        pushes = []
        drain = True
        if self._held is not None:
            if self._held.phys == phys:
                pushes.append(self._held)
                self._held = None
            elif self._held.phys > phys:
                drain = False  # everything queued behind it is later still
            else:
                self._held = None
        while drain:
            item = self._raw.get(0)
            if item is None:
                break
            push = self._push(item)
            if push.phys == phys:
                pushes.append(push)
            elif push.phys > phys:
                # No peek: hold the next step's first push for that step.
                self._held = push
                break

        by_path = {}
        for p in pushes:
            if p.path in self._subs:
                by_path.setdefault(p.path, []).append(p)
        arrays = {path: self._subs[path].assemble(plist) for path, plist in by_path.items()}
        if self.backpressure:
            self._raw.ack(phys)
        return Step(phys, wall_ns, arrays)

    @property
    def end_of_stream(self):
        """True once the writer has left and every step it committed has been returned."""
        return self._raw.end_of_stream()

    def steps(self, max_steps=None, timeout=None, idle_timeout=None, poll_ms=1000):
        """Yield Steps as the writer commits them.

        Iteration ends when the writer closes the file (or its process
        exits), after every step it committed has been yielded. A writer that
        is only paused keeps iteration waiting. It also ends on any bound you
        give it: max_steps steps, timeout seconds in total, or idle_timeout
        seconds without a step. Break out or interrupt it to stop sooner.

        The generator holds nothing that needs cleaning up; the File does.
        Close the File, or use it as a context manager, when you are done.
        """
        start = last = time.monotonic()
        count = 0
        while max_steps is None or count < max_steps:
            now = time.monotonic()
            wait_s = poll_ms / 1000
            if timeout is not None:
                wait_s = min(wait_s, start + timeout - now)
            if idle_timeout is not None:
                wait_s = min(wait_s, last + idle_timeout - now)
            if wait_s <= 0:
                return
            step = self.next_step(int(wait_s * 1000))
            if step is not None:
                count += 1
                last = time.monotonic()
                yield step
            elif self.end_of_stream:
                return

    def __iter__(self):
        return self.steps()

    def _sub(self, path):
        sub = self._subs.get(path)
        if sub is None:
            raise KeyError(f"{path!r} is not subscribed; call subscribe() first")
        return sub

    def _push(self, item):
        phys, path, start, count, buf = item
        sub = self._subs.get(path)
        data = sub.values(buf, count) if sub else np.frombuffer(buf, dtype=np.uint8)
        return Push(phys, path, start, data)

    def _discard_backlog(self):
        """Drop the step notifications queued before the first subscription.

        Those steps were committed before the writer knew of this reader, so
        they carry nothing for it. Nothing can have been pushed yet either.
        Returns the last step dropped, or None."""
        last = None
        while (ready := self._raw.wait_step_ready(0)) is not None:
            last = ready[0]
        return last


def open(path, backpressure=False):
    """Open a vol-stream file for reading. See File for backpressure."""
    return File(_volstream.open(path), backpressure)


def follow(path, selections=None, timeout_ms=10000, backpressure=False, deflate=None):
    """Open a live stream and subscribe to it in one call.

    selections is anything File.subscribe() takes. By default every dataset
    in the writer's schema with an integer or float type is subscribed.
    Waits up to timeout_ms for the writer's first committed step.

        with volstream.follow("run.h5") as stream:
            for step in stream.steps(max_steps=100):
                ...
    """
    f = open(path, backpressure)
    try:
        if selections is None:
            selections = [
                v.path
                for v in f.schema(timeout_ms).values()
                if not v.is_attr and v.dtype is not None and v.shape is not None
            ]
            if not selections:
                raise Error(f"{path!r}: the stream has no integer or float datasets to follow")
        f.subscribe(selections, timeout_ms, deflate)
    except BaseException:
        f.close()
        raise
    return f
