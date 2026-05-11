"""pystro stub for `_pickle` (C accelerator for pickle)."""


class PickleError(Exception):
    pass


class PicklingError(PickleError):
    pass


class UnpicklingError(PickleError):
    pass


class Pickler:
    def __init__(self, file, protocol=None, **kw):
        self._file = file
    def dump(self, obj):
        self._file.write(dumps(obj))


class Unpickler:
    def __init__(self, file, **kw):
        self._file = file
    def load(self):
        return loads(self._file.read())


class PickleBuffer:
    def __init__(self, buffer):
        self._buffer = buffer
    def raw(self):
        return self._buffer
    def release(self):
        self._buffer = None


# CPython's _pickle re-exports the same classes pickle does; pystro's
# pickle.py already provides them, but tests import via _pickle too.
def dumps(obj, protocol=None, *, fix_imports=True, buffer_callback=None):
    import pickle
    return pickle.dumps(obj, protocol)


def loads(b, *, fix_imports=True, encoding="ASCII", errors="strict", buffers=None):
    import pickle
    return pickle.loads(b)


def dump(obj, fp, protocol=None, *, fix_imports=True, buffer_callback=None):
    fp.write(dumps(obj, protocol))


def load(fp, *, fix_imports=True, encoding="ASCII", errors="strict", buffers=None):
    return loads(fp.read())


__all__ = ["PickleBuffer", "PickleError", "PicklingError", "UnpicklingError",
           "Pickler", "Unpickler", "dumps", "loads", "dump", "load"]
