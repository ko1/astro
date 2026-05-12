# pystro stdlib `io` (minimal).

class StringIO:
    def __init__(self, initial="", newline="\n"):
        # CPython StringIO accepts `newline` kwarg (translation control).
        # Pystro doesn't perform any newline translation; the kwarg is
        # accepted but ignored except `newline=''` which disables the
        # implicit `\n` separator on write.
        self._chunks = [initial] if initial else []
        self._closed = False
        self._pos = 0
        self.newlines = None
        self._newline = newline
    def write(self, s):
        if self._closed:
            raise ValueError("write on closed StringIO")
        if not isinstance(s, str):
            raise TypeError("write needs str")
        self._chunks.append(s)
        return len(s)
    def getvalue(self):
        return "".join(self._chunks)
    def read(self, size=-1):
        v = self.getvalue()
        if self._pos >= len(v): return ""
        if size < 0 or self._pos + size > len(v):
            r = v[self._pos:]
            self._pos = len(v)
        else:
            r = v[self._pos:self._pos + size]
            self._pos += size
        return r
    def readline(self):
        v = self.getvalue()
        if self._pos >= len(v): return ""
        nl = v.find("\n", self._pos)
        if nl < 0:
            r = v[self._pos:]
            self._pos = len(v)
        else:
            r = v[self._pos:nl + 1]
            self._pos = nl + 1
        return r
    def readlines(self):
        out = []
        while True:
            line = self.readline()
            if not line: break
            out.append(line)
        return out
    def __iter__(self):
        while True:
            line = self.readline()
            if not line: break
            yield line
    def seek(self, pos):
        self._pos = pos
    def tell(self):
        return self._pos
    def close(self):
        self._closed = True
    def __enter__(self):
        return self
    def __exit__(self, *a):
        self.close()


class BytesIO:
    def __init__(self, initial=None):
        self._chunks = []
        if initial:
            self._chunks.append(bytes(initial))
        self._closed = False
        self._pos = 0
    def write(self, b):
        if self._closed:
            raise ValueError("write on closed BytesIO")
        self._chunks.append(bytes(b))
        return len(b)
    def getvalue(self):
        if not self._chunks:
            return b""
        result = b""
        for c in self._chunks:
            result = result + c
        return result
    def read(self, size=-1):
        v = self.getvalue()
        if self._pos >= len(v): return b""
        if size < 0 or self._pos + size > len(v):
            r = v[self._pos:]
            self._pos = len(v)
        else:
            r = v[self._pos:self._pos + size]
            self._pos += size
        return r
    def seek(self, pos, whence=0):
        if whence == 0:
            self._pos = pos
        elif whence == 1:
            self._pos += pos
        else:
            self._pos = len(self.getvalue()) + pos
        return self._pos
    def tell(self):
        return self._pos
    def close(self):
        self._closed = True
    def __enter__(self):
        return self
    def __exit__(self, *a):
        self.close()
    def getbuffer(self):
        # Returns the underlying bytes for direct read access.  pystro
        # doesn't have a real memoryview-buffer protocol; return bytes.
        return self.getvalue()
    def readable(self): return True
    def writable(self): return True
    def seekable(self): return True
    def truncate(self, size=None):
        v = self.getvalue()
        if size is None: size = self._pos
        self._chunks = [v[:size]]
        return size
    def writelines(self, lines):
        for line in lines: self.write(line)
    def readline(self, size=-1):
        v = self.getvalue()
        if self._pos >= len(v): return b""
        nl = v.find(b"\n", self._pos)
        if nl < 0: end = len(v)
        else:      end = nl + 1
        if size >= 0: end = min(end, self._pos + size)
        r = v[self._pos:end]
        self._pos = end
        return r
    def readlines(self, hint=-1):
        out = []
        while True:
            line = self.readline()
            if not line: break
            out.append(line)
        return out
    def __iter__(self):
        while True:
            line = self.readline()
            if not line: return
            yield line
    @property
    def closed(self): return self._closed


# Many CPython libs reach for io.IOBase / TextIOWrapper / BufferedReader
# etc as base classes / type-checks.  We expose minimal placeholders.
class IOBase:
    def close(self): pass
    def __enter__(self): return self
    def __exit__(self, *a): self.close()
    def readable(self): return False
    def writable(self): return False
    def seekable(self): return False
    def fileno(self): raise OSError("no fileno")
    def isatty(self): return False
    def flush(self): pass
    def closed(self): return False


class RawIOBase(IOBase): pass
class BufferedIOBase(IOBase): pass
class TextIOBase(IOBase): pass


class FileIO(RawIOBase):
    def __init__(self, *a, **kw): pass


class BufferedReader(BufferedIOBase):
    def __init__(self, raw, buffer_size=8192): self.raw = raw
class BufferedWriter(BufferedIOBase):
    def __init__(self, raw, buffer_size=8192): self.raw = raw
class BufferedRandom(BufferedIOBase):
    def __init__(self, raw, buffer_size=8192): self.raw = raw
class BufferedRWPair(BufferedIOBase):
    def __init__(self, reader, writer): self.reader = reader; self.writer = writer


class TextIOWrapper(TextIOBase):
    def __init__(self, buffer, encoding=None, errors=None, newline=None,
                 line_buffering=False, write_through=False):
        self.buffer = buffer
        self.encoding = encoding or "utf-8"
        self.errors = errors or "strict"
        self.newline = newline
    def read(self, size=-1): return ""
    def readline(self): return ""
    def write(self, s): return len(s)
    def close(self):
        if self.buffer: self.buffer.close()


# Default newline / buffer sizes.
DEFAULT_BUFFER_SIZE = 8192
SEEK_SET = 0
SEEK_CUR = 1
SEEK_END = 2


class UnsupportedOperation(OSError, ValueError):
    pass


# Re-export `open` from builtins so `io.open(...)` works.
open = open


__all__ = ["StringIO", "BytesIO", "IOBase", "RawIOBase", "BufferedIOBase",
           "TextIOBase", "FileIO", "BufferedReader", "BufferedWriter",
           "BufferedRandom", "BufferedRWPair", "TextIOWrapper",
           "DEFAULT_BUFFER_SIZE", "SEEK_SET", "SEEK_CUR", "SEEK_END",
           "UnsupportedOperation", "open"]
