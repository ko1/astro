# pystro stdlib `hashlib` (md5, sha256 — backed by C builtins).

class _Hash:
    def __init__(self, name, hexfn, data=None):
        self._name = name
        self._hexfn = hexfn
        self._chunks = []
        if data is not None:
            self.update(data)
    def update(self, data):
        if isinstance(data, str):
            data = data.encode()
        self._chunks.append(data)
    def _whole(self):
        # Concatenate chunks (bytes) into one bytes.
        if len(self._chunks) == 0:
            return b""
        if len(self._chunks) == 1:
            return self._chunks[0]
        out = b""
        for c in self._chunks:
            out = out + c
        return out
    def hexdigest(self):
        return self._hexfn(self._whole())
    def digest(self):
        h = self.hexdigest()
        # Convert hex string to bytes.
        out = b""
        for i in range(0, len(h), 2):
            byte_str = h[i:i+2]
            byte_val = int(byte_str, 16)
            out = out + bytes([byte_val])
        return out


def md5(data=None):
    return _Hash("md5", __pystro_md5__, data)


def sha256(data=None):
    return _Hash("sha256", __pystro_sha256__, data)


def new(name, data=None):
    if name == "md5":    return md5(data)
    if name == "sha256": return sha256(data)
    raise ValueError("unknown hash: " + name)


__all__ = ["md5", "sha256", "new"]
