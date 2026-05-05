# pystro stdlib `tempfile` (minimal).
import os
import random


def gettempdir():
    for v in ("TMPDIR", "TEMP", "TMP"):
        d = os.environ.get(v)
        if d:
            return d
    return "/tmp"


def _make_name(prefix, suffix):
    n = random.randint(0, 0x7fffffff)
    return prefix + str(n) + suffix


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
    def __init__(self, mode="w+b", suffix="", prefix="tmp", dir=None, delete=True):
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
    def close(self):
        try: self._f.close()
        except Exception: pass
        if self._delete and os.path.exists(self.name):
            try: os.remove(self.name)
            except Exception: pass


class TemporaryFile(NamedTemporaryFile):
    pass
