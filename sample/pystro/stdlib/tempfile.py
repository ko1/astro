# pystro stdlib `tempfile` (minimal).
import os
import random

# Open-flag aliases CPython exposes for tests that probe tempfile
# internals; the real CPython uses these in os.open() but pystro hands
# files via plain open() so the values are informational only.
_text_openflags = getattr(os, "O_RDWR", 0) | getattr(os, "O_CREAT", 0) | getattr(os, "O_EXCL", 0)
_bin_openflags = _text_openflags


def gettempdir():
    for v in ("TMPDIR", "TEMP", "TMP"):
        d = os.environ.get(v)
        if d:
            return d
    return "/tmp"


def _make_name(prefix, suffix):
    n = random.randint(0, 0x7fffffff)
    return prefix + str(n) + suffix


class _RandomNameSequence:
    """Iterator that yields random tempfile-name suffixes; CPython
    tests probe this directly for reproducibility."""
    def __init__(self):
        self.rng = random
        self.characters = ("abcdefghijklmnopqrstuvwxyz"
                           "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")
    def __iter__(self):
        return self
    def __next__(self):
        c = self.characters
        return "".join(self.rng.choice(c) for _ in range(8))


_RANDOM_NAME_LEN = 8


def mkstemp(suffix="", prefix="tmp", dir=None, text=False):
    if dir is None: dir = gettempdir()
    for _ in range(100):
        name = os.path.join(dir, _make_name(prefix, suffix))
        if not os.path.exists(name):
            mode = "w" if text else "wb"
            f = open(name, mode)
            return (f, name)
    raise FileExistsError("tempfile: could not generate unique name")


def mkdtemp(suffix="", prefix="tmp", dir=None):
    if dir is None: dir = gettempdir()
    for _ in range(100):
        name = os.path.join(dir, _make_name(prefix, suffix))
        if not os.path.exists(name):
            os.mkdir(name)
            return name
    raise FileExistsError("mkdtemp: could not create unique directory")


class NamedTemporaryFile:
    def __init__(self, mode="w+b", buffering=-1, encoding=None, newline=None,
                 suffix="", prefix="tmp", dir=None, delete=True, *,
                 errors=None, delete_on_close=True):
        if dir is None: dir = gettempdir()
        self.name = os.path.join(dir, _make_name(prefix, suffix))
        self._f = open(self.name, mode)
        self._delete = delete
    def __enter__(self):
        return self
    def __exit__(self, *args):
        self.close()
        return False
    def write(self, s): return self._f.write(s)
    def read(self, n=-1): return self._f.read(n) if n >= 0 else self._f.read()
    def readline(self): return self._f.readline()
    def seek(self, n): return self._f.seek(n) if hasattr(self._f, "seek") else None
    def tell(self): return self._f.tell() if hasattr(self._f, "tell") else 0
    def flush(self): return self._f.flush() if hasattr(self._f, "flush") else None
    def truncate(self, *a): return self._f.truncate(*a) if hasattr(self._f, "truncate") else None
    def __iter__(self): return iter(self._f)
    def __next__(self):
        line = self.readline()
        if not line: raise StopIteration
        return line
    def fileno(self): return self._f.fileno() if hasattr(self._f, "fileno") else -1
    @property
    def file(self): return self._f
    def close(self):
        try: self._f.close()
        except Exception: pass
        if self._delete and os.path.exists(self.name):
            try: os.remove(self.name)
            except Exception: pass


class TemporaryFile(NamedTemporaryFile):
    pass


_TemporaryFileWrapper = NamedTemporaryFile


class SpooledTemporaryFile(NamedTemporaryFile):
    def __init__(self, max_size=0, mode="w+b", **kw):
        super().__init__(mode=mode, **kw)


def mktemp(suffix="", prefix="tmp", dir=None):
    """Deprecated; pystro stub returns a unique-ish name without
    creating the file."""
    if dir is None: dir = gettempdir()
    return os.path.join(dir, _make_name(prefix, suffix))


class TemporaryDirectory:
    def __init__(self, suffix="", prefix="tmp", dir=None):
        self.name = mkdtemp(suffix, prefix, dir)
    def __enter__(self): return self.name
    def __exit__(self, *exc):
        try: os.rmdir(self.name)
        except Exception: pass
    def cleanup(self):
        try: os.rmdir(self.name)
        except Exception: pass
