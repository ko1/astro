# pystro stdlib `io` (minimal).

class StringIO:
    def __init__(self, initial=""):
        self._chunks = [initial] if initial else []
        self._closed = False
        self._pos = 0
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


__all__ = ["StringIO", "BytesIO"]
