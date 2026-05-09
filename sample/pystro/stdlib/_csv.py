"""pystro stub for `_csv` (C-extension behind csv module)."""

QUOTE_MINIMAL = 0
QUOTE_ALL = 1
QUOTE_NONNUMERIC = 2
QUOTE_NONE = 3
QUOTE_STRINGS = 4
QUOTE_NOTNULL = 5


class Error(Exception):
    pass


_dialects = {}


def list_dialects():
    return list(_dialects)


def register_dialect(name, dialect=None, **fmtparams):
    _dialects[name] = (dialect, fmtparams)


def unregister_dialect(name):
    del _dialects[name]


def get_dialect(name):
    return _dialects.get(name)


class Dialect:
    delimiter = ","
    quotechar = '"'
    escapechar = None
    doublequote = True
    skipinitialspace = False
    lineterminator = "\r\n"
    quoting = QUOTE_MINIMAL
    strict = False


__version__ = "1.0"


def reader(csvfile, dialect="excel", **fmtparams):
    """Minimal CSV reader — splits on commas, no quote handling."""
    class _R:
        def __init__(self): self._lines = iter(csvfile)
        def __iter__(self): return self
        def __next__(self):
            line = next(self._lines).rstrip("\r\n")
            return line.split(",")
        @property
        def line_num(self): return 0
        @property
        def dialect(self): return Dialect
    return _R()


def writer(csvfile, dialect="excel", **fmtparams):
    class _W:
        def writerow(self, row):
            csvfile.write(",".join(str(x) for x in row) + "\r\n")
            return len(row)
        def writerows(self, rows):
            for r in rows: self.writerow(r)
        @property
        def dialect(self): return Dialect
    return _W()


_field_size_limit = 131072


def field_size_limit(*args):
    global _field_size_limit
    if args:
        old = _field_size_limit
        _field_size_limit = args[0]
        return old
    return _field_size_limit


__all__ = [
    "QUOTE_MINIMAL", "QUOTE_ALL", "QUOTE_NONNUMERIC", "QUOTE_NONE",
    "QUOTE_STRINGS", "QUOTE_NOTNULL",
    "Error", "Dialect", "reader", "writer",
    "register_dialect", "unregister_dialect", "list_dialects", "get_dialect",
    "field_size_limit", "__version__",
]
