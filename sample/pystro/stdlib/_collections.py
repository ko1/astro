"""pystro stub for `_collections` (CPython C accelerator for `collections`)."""

from collections import deque, defaultdict, OrderedDict


class _deque_iterator:
    def __init__(self, dq, idx=0):
        self.dq = dq
        self.idx = idx
    def __iter__(self): return self
    def __next__(self):
        if self.idx >= len(self.dq): raise StopIteration
        v = self.dq[self.idx]
        self.idx += 1
        return v


# `OrderedDict` named in CPython as `collections.OrderedDict`; some
# code imports it from `_collections`.
__all__ = ["deque", "defaultdict", "OrderedDict", "_deque_iterator"]
