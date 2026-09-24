"""Python subscriber for the vol-stream HDF5 connector.

See docs/python-plan.md for the design and the milestones.
"""

from ._volstream import Error, File, open

__all__ = ["Error", "File", "open"]
