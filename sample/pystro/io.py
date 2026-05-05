# pystro stdlib `io` (minimal).

class StringIO:
    def __init__(self, initial=""):
        self._chunks = [initial] if initial else []
        self._closed = False
    def write(self, s):
        if self._closed:
            raise ValueError("write on closed StringIO")
        if not isinstance(s, str):
            raise TypeError("write needs str")
        self._chunks.append(s)
        return len(s)
    def getvalue(self):
        return "".join(self._chunks)
    def read(self):
        return self.getvalue()
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
            self._chunks.append(initial)
        self._closed = False
    def write(self, b):
        if self._closed:
            raise ValueError("write on closed BytesIO")
        self._chunks.append(b)
        return len(b)
    def getvalue(self):
        # Concatenate all chunks into one bytes.
        if not self._chunks:
            return b""
        result = b""
        for c in self._chunks:
            result = result + c
        return result
    def read(self):
        return self.getvalue()
    def close(self):
        self._closed = True
    def __enter__(self):
        return self
    def __exit__(self, *a):
        self.close()


__all__ = ["StringIO", "BytesIO"]
