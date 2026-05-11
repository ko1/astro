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


# Forward dump / dumps / load / loads / Pickler / Unpickler to the
# pure-Python implementations inside pickle.py.  Tests that import
# from _pickle directly (e.g. test_pickle) need these to be present;
# the previous stub recursed by calling pickle.dumps which forms a
# cycle, so route through pickle._dumps / _Pickler / etc. (the
# rename pickle.py uses for the pure-Python path).
def _pure(name):
    import pickle as _pk
    return getattr(_pk, "_" + name, getattr(_pk, name, None))


def dump(obj, fp, protocol=None, *, fix_imports=True, buffer_callback=None):
    fn = _pure("dump")
    return fn(obj, fp, protocol, fix_imports=fix_imports,
              buffer_callback=buffer_callback) if fn else None


def dumps(obj, protocol=None, *, fix_imports=True, buffer_callback=None):
    fn = _pure("dumps")
    return fn(obj, protocol, fix_imports=fix_imports,
              buffer_callback=buffer_callback) if fn else None


def load(fp, *, fix_imports=True, encoding="ASCII", errors="strict", buffers=None):
    fn = _pure("load")
    return fn(fp, fix_imports=fix_imports, encoding=encoding,
              errors=errors, buffers=buffers) if fn else None


def loads(b, *, fix_imports=True, encoding="ASCII", errors="strict", buffers=None):
    fn = _pure("loads")
    return fn(b, fix_imports=fix_imports, encoding=encoding,
              errors=errors, buffers=buffers) if fn else None


def _pickler_class():
    import pickle as _pk
    return getattr(_pk, "_Pickler", None)


def _unpickler_class():
    import pickle as _pk
    return getattr(_pk, "_Unpickler", None)


class Pickler:
    def __new__(cls, *args, **kwargs):
        target = _pickler_class()
        if target is None:
            return super().__new__(cls)
        return target(*args, **kwargs)


class Unpickler:
    def __new__(cls, *args, **kwargs):
        target = _unpickler_class()
        if target is None:
            return super().__new__(cls)
        return target(*args, **kwargs)


__all__ = ["PickleBuffer", "PickleError", "PicklingError", "UnpicklingError",
           "Pickler", "Unpickler", "dump", "dumps", "load", "loads"]
