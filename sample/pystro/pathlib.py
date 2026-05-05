# pystro stdlib `pathlib` (minimal).

import os as _os


class Path:
    def __init__(self, *parts):
        # Concatenate parts using '/'.
        if len(parts) == 0:
            self._s = ""
        elif len(parts) == 1:
            v = parts[0]
            self._s = v._s if isinstance(v, Path) else v
        else:
            self._s = _os.path.join(*[(p._s if isinstance(p, Path) else p) for p in parts])
    def __str__(self):
        return self._s
    def __repr__(self):
        return "PosixPath(" + repr(self._s) + ")"
    def __eq__(self, other):
        if isinstance(other, Path):
            return self._s == other._s
        return False
    def __hash__(self):
        return hash(self._s)
    def __truediv__(self, other):
        if isinstance(other, Path):
            other = other._s
        return Path(_os.path.join(self._s, other))

    def exists(self):  return _os.path.exists(self._s)
    def is_dir(self):  return _os.path.isdir(self._s)
    def is_file(self): return _os.path.isfile(self._s)
    def is_absolute(self): return _os.path.isabs(self._s)
    def absolute(self):    return Path(_os.path.abspath(self._s))
    def parent(self):  return Path(_os.path.dirname(self._s))
    def name(self):    return _os.path.basename(self._s)
    def suffix(self):  return _os.path.splitext(self._s)[1]
    def stem(self):
        bn = _os.path.basename(self._s)
        return _os.path.splitext(bn)[0]

    def read_text(self):
        with open(self._s) as f:
            return f.read()

    def write_text(self, s):
        with open(self._s, "w") as f:
            f.write(s)

    def read_bytes(self):
        with open(self._s, "rb") as f:
            return f.read()

    def write_bytes(self, b):
        with open(self._s, "wb") as f:
            f.write(b)

    def iterdir(self):
        for n in _os.listdir(self._s):
            yield Path(_os.path.join(self._s, n))


PosixPath = Path
WindowsPath = Path

__all__ = ["Path", "PosixPath"]
