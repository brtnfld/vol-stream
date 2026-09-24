"""A PyTorch IterableDataset over a live vol-stream. Needs torch.

    from volstream.torch import StreamDataset
    ds = StreamDataset("run.h5", "/entry/data/data", max_steps=1000)
    for batch in torch.utils.data.DataLoader(ds, batch_size=8):
        ...
"""

from torch.utils.data import IterableDataset, get_worker_info

from ._file import follow


class StreamDataset(IterableDataset):
    """Yields one item per committed step of a live stream.

    Subscribes when constructed, so no step committed after that is missed,
    and drains only when iterated. With one subscribed path each item is that
    path's array; with several it is a dict of them. Steps that carry nothing
    for any subscribed path are skipped. An array can be a numpy masked array
    when some elements did not arrive (see volstream.Step); the default
    collate function does not accept those, so give a collate_fn or a
    transform that handles them if your subscription can produce them.

    Use num_workers=0. A stream is one ordered source, and a subscription
    belongs to the process that made it, so the dataset refuses to run in a
    DataLoader worker or to be copied into one. Put parallel work in the
    training loop or the transform instead.

    This does not apply backpressure: the writer does not wait for a slow
    consumer.

    Iteration ends when the writer closes the file, or on max_steps, timeout
    or idle_timeout, as in volstream.File.steps().
    """

    def __init__(self, path, selections=None, *, transform=None, max_steps=None, timeout=None,
                 idle_timeout=None, timeout_ms=10000):
        super().__init__()
        self._file = follow(path, selections, timeout_ms)
        self._transform = transform
        self._bounds = dict(max_steps=max_steps, timeout=timeout, idle_timeout=idle_timeout)

    def __iter__(self):
        if get_worker_info() is not None:
            raise RuntimeError("StreamDataset must be used with num_workers=0; see its docstring")
        for step in self._file.steps(**self._bounds):
            if not step.arrays:
                continue
            item = next(iter(step.arrays.values())) if len(step.arrays) == 1 else dict(step.arrays)
            yield self._transform(item) if self._transform else item

    def __getstate__(self):
        raise TypeError("a StreamDataset holds a live subscription and cannot be copied to another process; "
                        "use num_workers=0")

    def close(self):
        self._file.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
