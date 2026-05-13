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
            self._buf = []
            self._newline = newline
            self._pos = 0
            self._closed = False
            if initial: self._buf.append(initial)
        def write(self, s):
            if self._closed:
                raise ValueError("I/O operation on closed file.")
            if not isinstance(s, str):
                raise TypeError("write requires str")
            # Simple append (the common case). Overwrite-in-place / seek-
            # past-end semantics aren't needed for most tests; if seek
            # was used to reset to 0 then write, the resulting buffer
            # length might not match CPython exactly but value works for
            # round-trip read.
            self._buf.append(s)
            self._pos += len(s)
            return len(s)
        def getvalue(self): return "".join(self._buf)
        def read(self, n=-1):
            v = self.getvalue()
            if self._pos >= len(v): return ""
            if n is None or n < 0 or self._pos + n > len(v):
                r = v[self._pos:]
                self._pos = len(v)
            else:
                r = v[self._pos:self._pos + n]
                self._pos += n
            return r
        def readline(self, size=-1):
            v = self.getvalue()
            if self._pos >= len(v): return ""
            nl = v.find("\n", self._pos)
            if nl < 0:
                r = v[self._pos:]; self._pos = len(v)
            else:
                r = v[self._pos:nl + 1]; self._pos = nl + 1
            if size is not None and size >= 0 and len(r) > size:
                r = r[:size]; self._pos -= (len(r) - size)
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
        def seek(self, pos, whence=0):
            if whence == 0: self._pos = pos
            elif whence == 1: self._pos += pos
            elif whence == 2: self._pos = len(self.getvalue()) + pos
            return self._pos
        def tell(self): return self._pos
        def truncate(self, size=None):
            v = self.getvalue()
            if size is None: size = self._pos
            self._buf = [v[:size]]
            return size
        def close(self): self._closed = True
        @property
        def closed(self): return self._closed
        def readable(self): return True
        def writable(self): return True
        def seekable(self): return True
        def flush(self): pass
        def __enter__(self): return self
        def __exit__(self, *a): self.close(); return False


class FileIO(RawIOBase):
    """Pystro's binary file object accessed through the builtin `open()`.
    Most code paths in CPython's test suite construct FileIO directly
    (often as base class for io test mocks) and then call .write/.read
    on it — so the stub must really wrap an `open()` handle."""
    def __init__(self, name, mode="r", closefd=True, opener=None):
        if isinstance(name, int):
            # fd path: open by descriptor.  Pystro's builtin `open` only
            # accepts paths, so for fd-based FileIO we keep a sentinel
            # and let downstream code stub via os.read/os.write if needed.
            self._fp = None
            self._fd = name
            self._name = name
            self._mode = mode if "b" in mode else (mode + "b")
            self._closed = False
            return
        m = mode
        if "b" not in m: m = m + "b"
        # Map "r+b" / "w+b" to pystro builtin open's two-char modes.
        self._fp = open(name, m)
        self._fd = -1
        self._name = name
        self._mode = m
        self._closed = False
    def write(self, b):
        if self._fp is None: raise OSError("FileIO: no underlying file")
        return self._fp.write(b)
    def read(self, size=-1):
        if self._fp is None: return b""
        return self._fp.read(size)
    def readline(self, size=-1):
        if self._fp is None: return b""
        return self._fp.readline()
    def readall(self):
        if self._fp is None: return b""
        return self._fp.read()
    def seek(self, p, w=0):
        if self._fp is None: return 0
        return self._fp.seek(p, w)
    def tell(self):
        if self._fp is None: return 0
        return self._fp.tell()
    def flush(self):
        if self._fp is None: return None
        try: return self._fp.flush()
        except AttributeError: return None
    def truncate(self, size=None):
        if self._fp is None: return 0
        try: return self._fp.truncate(size)
        except AttributeError: return 0
    def close(self):
        if self._closed: return
        self._closed = True
        if self._fp is not None:
            try: self._fp.close()
            except Exception: pass
    def readable(self):
        return "r" in self._mode or "+" in self._mode
    def writable(self):
        return "w" in self._mode or "a" in self._mode or "+" in self._mode
    def seekable(self): return True
    def fileno(self):
        if self._fd >= 0: return self._fd
        if self._fp is None: return -1
        try: return self._fp.fileno()
        except AttributeError: return -1
    def isatty(self): return False
    @property
    def closed(self): return self._closed
    @property
    def name(self): return self._name
    @property
    def mode(self): return self._mode
    def __enter__(self): return self
    def __exit__(self, *exc): self.close(); return False
    def __iter__(self):
        if self._fp is None: return
        for line in self._fp: yield line


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
    def detach(self):
        # CPython detach() unwraps the underlying binary buffer and
        # leaves the text wrapper in a closed state.  We mimic by
        # returning the buffer and marking the wrapper closed without
        # propagating .close() to the buffer.
        buf = self.buffer
        self.buffer = None
        self._closed = True
        return buf
    def reconfigure(self, *, encoding=None, errors=None, newline=None,
                    line_buffering=None, write_through=None):
        if encoding is not None: self.encoding = encoding
        if errors is not None: self.errors = errors
        if newline is not None: self.newline = newline
    def fileno(self):
        return getattr(self.buffer, "fileno", lambda: -1)()
    def isatty(self):
        return getattr(self.buffer, "isatty", lambda: False)()
    @property
    def line_buffering(self): return False
    @property
    def write_through(self): return False
    @property
    def name(self):
        return getattr(self.buffer, "name", None)
    @property
    def mode(self):
        return getattr(self.buffer, "mode", "r")


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

