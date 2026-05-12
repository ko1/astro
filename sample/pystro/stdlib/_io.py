"""pystro stub for `_io` (the C accelerator for io).

CPython's `io.py` imports a long list of names from `_io`; we mirror it
here as plain Python stubs so test code that says `from io import IOBase`
or `import _io` works.  pystro's own io.py provides richer behavior for
StringIO / BytesIO; the rest are abstract / minimal."""


DEFAULT_BUFFER_SIZE = 8192


class UnsupportedOperation(OSError, ValueError):
    pass


class BlockingIOError(OSError):
    def __init__(self, *args):
        super().__init__(*args)
        self.characters_written = 0


class IOBase:
    """Abstract base for all I/O classes."""
    closed = False
    def close(self): self.closed = True
    def fileno(self): raise UnsupportedOperation("fileno")
    def flush(self): pass
    def isatty(self): return False
    def readable(self): return False
    def writable(self): return False
    def seekable(self): return False
    def seek(self, *a, **k): raise UnsupportedOperation("seek")
    def tell(self): raise UnsupportedOperation("tell")
    def truncate(self, *a): raise UnsupportedOperation("truncate")
    def __enter__(self): return self
    def __exit__(self, *a): self.close()
    def __iter__(self): return self
    def __next__(self):
        line = self.readline()
        if not line: raise StopIteration
        return line
    def readline(self, size=-1): return ""
    def readlines(self, hint=-1): return list(self)
    def writelines(self, lines):
        for line in lines: self.write(line)


class RawIOBase(IOBase):
    pass


class BufferedIOBase(IOBase):
    pass


class TextIOBase(IOBase):
    pass


# Richer types that actually exercise data — pystro io.py defines these.
# Try to inherit from the io.py versions if available, else stub.
try:
    import io as _io_real
    BytesIO = _io_real.BytesIO
    StringIO = _io_real.StringIO
except (ImportError, AttributeError):
    class BytesIO(BufferedIOBase):
        def __init__(self, initial=b""):
            self._buf = bytearray(initial)
            self._pos = 0
            self._closed = False
        def read(self, n=-1):
            if n is None or n < 0: n = len(self._buf) - self._pos
            r = bytes(self._buf[self._pos:self._pos+n])
            self._pos += len(r); return r
        def read1(self, n=-1): return self.read(n)
        def write(self, b):
            self._buf[self._pos:self._pos+len(b)] = b
            self._pos += len(b); return len(b)
        def writelines(self, lines):
            for line in lines: self.write(line)
        def getvalue(self): return bytes(self._buf)
        def getbuffer(self):
            # CPython returns a memoryview; pystro returns the bytearray
            # (mutable view of the underlying buffer).
            return self._buf
        def seek(self, p, w=0):
            if w == 0: self._pos = p
            elif w == 1: self._pos += p
            else: self._pos = len(self._buf) + p
            return self._pos
        def tell(self): return self._pos
        def truncate(self, size=None):
            if size is None: size = self._pos
            del self._buf[size:]
            return size
        def readable(self): return True
        def writable(self): return True
        def seekable(self): return True
        def readline(self, size=-1):
            if self._pos >= len(self._buf): return b""
            nl = self._buf.find(b"\n", self._pos)
            end = (nl + 1) if nl >= 0 else len(self._buf)
            if size >= 0: end = min(end, self._pos + size)
            r = bytes(self._buf[self._pos:end]); self._pos = end
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
        def close(self): self._closed = True
        @property
        def closed(self): return self._closed

    class StringIO(TextIOBase):
        def __init__(self, initial="", newline="\n"):
            # `newline` kwarg accepted for CPython parity (e.g. email
            # feedparser uses `StringIO(newline='')`); pystro doesn't
            # perform newline translation.
            self._buf = []
            self._newline = newline
            if initial: self._buf.append(initial)
        def write(self, s):
            self._buf.append(s); return len(s)
        def getvalue(self): return "".join(self._buf)
        def read(self, n=-1):
            v = self.getvalue(); self._buf = [v[len(v):]]; return v
        def readable(self): return True
        def writable(self): return True


class FileIO(RawIOBase):
    """Stub: real file I/O routes through `open()` builtin."""
    def __init__(self, *a, **k): pass


class _BufferedBase(BufferedIOBase):
    """Common pass-through wrapper.  CPython's BufferedReader / Writer
    / Random aggregate a raw FileIO; the stub just delegates everything
    to .raw so user code that does `BufferedWriter(open(path,'wb')).write(b)`
    actually writes.  Read/write are bytes-mode pass-throughs."""
    def __init__(self, raw, *a, **k):
        self.raw = raw
    def read(self, size=-1): return self.raw.read(size)
    def readline(self, size=-1): return self.raw.readline()
    def readlines(self, hint=-1):
        try: return self.raw.readlines()
        except AttributeError:
            out = []
            while True:
                line = self.readline()
                if not line: break
                out.append(line)
            return out
    def write(self, b):
        return self.raw.write(b)
    def flush(self):
        try: return self.raw.flush()
        except AttributeError: return None
    def close(self):
        try: return self.raw.close()
        except AttributeError: return None
    @property
    def closed(self):
        try: return self.raw.closed
        except AttributeError: return False
    def seek(self, *a):
        return self.raw.seek(*a)
    def tell(self):
        return self.raw.tell()
    def fileno(self):
        try: return self.raw.fileno()
        except AttributeError: return -1
    def readable(self): return True
    def writable(self): return True
    def seekable(self): return True
    def __iter__(self):
        while True:
            line = self.readline()
            if not line: break
            yield line


class BufferedReader(_BufferedBase): pass
class BufferedWriter(_BufferedBase): pass
class BufferedRandom(_BufferedBase): pass


class BufferedRWPair(BufferedIOBase):
    def __init__(self, reader, writer, *a, **k):
        self.reader = reader; self.writer = writer
    def read(self, size=-1): return self.reader.read(size)
    def write(self, b): return self.writer.write(b)
    def close(self):
        try: self.reader.close()
        except AttributeError: pass
        try: self.writer.close()
        except AttributeError: pass


class TextIOWrapper(TextIOBase):
    def __init__(self, buffer, *a, **k):
        self.buffer = buffer
        self.encoding = k.get("encoding", "utf-8")
        self.errors = k.get("errors", "strict")
        self.newline = k.get("newline", None)
        self._closed = False

    def _decode(self, b):
        if isinstance(b, str): return b
        if isinstance(b, (bytes, bytearray)):
            try: return bytes(b).decode(self.encoding, self.errors)
            except Exception: return ""
        return str(b)

    def read(self, size=-1):
        return self._decode(self.buffer.read(size))

    def readline(self, size=-1):
        return self._decode(self.buffer.readline())

    def readlines(self, hint=-1):
        return self.read().splitlines(keepends=True)

    def write(self, s):
        if isinstance(s, str):
            self.buffer.write(s.encode(self.encoding, self.errors))
        else:
            self.buffer.write(bytes(s))
        return len(s)

    def writable(self): return True
    def readable(self): return True
    def seekable(self): return getattr(self.buffer, "seekable", lambda: False)()
    def seek(self, *a): return self.buffer.seek(*a)
    def tell(self): return self.buffer.tell()
    def flush(self):
        try: return self.buffer.flush()
        except AttributeError: return None
    def close(self):
        self._closed = True
        try: self.buffer.close()
        except AttributeError: pass
    @property
    def closed(self): return self._closed
    def __iter__(self):
        while True:
            line = self.readline()
            if not line: break
            yield line
    def __enter__(self): return self
    def __exit__(self, *exc): self.close(); return False


class IncrementalNewlineDecoder:
    def __init__(self, *a, **k): pass
    def decode(self, s, final=False): return s


def text_encoding(encoding, stacklevel=2):
    # PEP 597 (3.10+) — return a default encoding when None.  CPython's
    # builtin is C; pystro's userland can return "utf-8" / "locale".
    if encoding is None:
        return "utf-8"
    return encoding


# Re-export `builtins.open` directly so `io.open` (= `_io.open` in
# CPython) is a free function, not a Python def — placing it as a class
# attribute (e.g. `class T: open = io.open`) should not produce a bound
# method.
import builtins as _builtins
open = _builtins.open


def open_code(path):
    return open(path, "rb")


__all__ = [
    "IOBase", "RawIOBase", "BufferedIOBase", "TextIOBase",
    "FileIO", "BytesIO", "StringIO", "BufferedReader", "BufferedWriter",
    "BufferedRandom", "BufferedRWPair", "TextIOWrapper",
    "IncrementalNewlineDecoder", "UnsupportedOperation",
    "BlockingIOError", "DEFAULT_BUFFER_SIZE", "open", "open_code",
]

# Internal aliases used by CPython unittest / pathlib (the underscore
# variants point at the same classes — CPython exposes both).
_IOBase = IOBase
_RawIOBase = RawIOBase
_BufferedIOBase = BufferedIOBase
_TextIOBase = TextIOBase

# Stub for Windows-only io class — CPython references it on Linux too
# (just doesn't construct).  Returns a class object so isinstance / type
# probes don't crash.
class _WindowsConsoleIO(RawIOBase):
    def __init__(self, *a, **k):
        raise OSError("not supported on this platform")

# Internal aliases used by CPython unittest / pathlib (the underscore
# variants point at the same classes — CPython exposes both).
_IOBase = IOBase
_RawIOBase = RawIOBase
_BufferedIOBase = BufferedIOBase
_TextIOBase = TextIOBase

class _WindowsConsoleIO(RawIOBase):
    def __init__(self, *a, **k):
        raise OSError("not supported on this platform")

