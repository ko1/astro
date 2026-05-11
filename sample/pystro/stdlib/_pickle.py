"""pystro stub for `_pickle` (C accelerator for pickle).

Only expose the C-side-introduced classes (PickleBuffer / errors).
Do NOT shadow dumps / loads / dump / load / Pickler / Unpickler —
CPython's pickle.py does `from _pickle import dumps, ...` inside a
try/except ImportError, and falls back to the pure-Python _Pickler
implementation when this import fails.  Pystro's stub had its
own dumps/loads that recursively called pickle.dumps, which formed
an infinite loop after pickle.dumps was bound to it (the pure-Python
path is fine; the recursive stub was not).
"""


class PickleError(Exception):
    pass


class PicklingError(PickleError):
    pass


class UnpicklingError(PickleError):
    pass


class PickleBuffer:
    def __init__(self, buffer):
        self._buffer = buffer
    def raw(self):
        return self._buffer
    def release(self):
        self._buffer = None


__all__ = ["PickleBuffer", "PickleError", "PicklingError", "UnpicklingError"]
