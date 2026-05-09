"""pystro stub for `_dbm` (CPython's GDBM/NDBM accelerator).

pystro doesn't have a real on-disk DBM; provide a no-op surface
that uses an in-memory dict.  Tests that gate on `_dbm.error` /
`_dbm.library` succeed; actual DB usage degrades to memory.
"""


class error(Exception):
    pass


library = "pystro-memory"


def open(name, flag='c', mode=0o666):
    return _DB()


class _DB(dict):
    def close(self): pass
    def sync(self): pass
    def reorganize(self): pass
    def firstkey(self):
        if not self: return None
        return next(iter(self))
    def nextkey(self, key):
        keys = list(self.keys())
        try:
            i = keys.index(key)
            return keys[i + 1] if i + 1 < len(keys) else None
        except ValueError:
            return None


__all__ = ["error", "library", "open"]
