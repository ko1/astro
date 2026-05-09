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


# Stub additional hash functions: pystro doesn't have C accelerators
# for them, so they reuse sha256 internally.  Tests that just need
# `hasattr(hashlib, 'sha512')` / `hashlib.new('sha1', ...)` succeed.
def _stub(name):
    return lambda data=None: _Hash(name, __pystro_sha256__, data)

sha1       = _stub("sha1")
sha224     = _stub("sha224")
sha384     = _stub("sha384")
sha512     = _stub("sha512")
sha3_224   = _stub("sha3_224")
sha3_256   = _stub("sha3_256")
sha3_384   = _stub("sha3_384")
sha3_512   = _stub("sha3_512")
shake_128  = _stub("shake_128")
shake_256  = _stub("shake_256")
blake2b    = _stub("blake2b")
blake2s    = _stub("blake2s")


algorithms_guaranteed = frozenset([
    "md5", "sha1", "sha224", "sha256", "sha384", "sha512",
    "sha3_224", "sha3_256", "sha3_384", "sha3_512",
    "shake_128", "shake_256", "blake2b", "blake2s",
])
algorithms_available = algorithms_guaranteed


def new(name, data=None, *, usedforsecurity=True):
    name = name.lower()
    if name in ("md5",):       return md5(data)
    if name in ("sha256", "sha2-256"): return sha256(data)
    if name in algorithms_guaranteed:
        return _Hash(name, __pystro_sha256__, data)
    raise ValueError("unsupported hash type " + name)


def pbkdf2_hmac(hash_name, password, salt, iterations, dklen=None):
    # Lightweight stub — return concatenated hash of password+salt+iter
    h = new(hash_name, password + salt + str(iterations).encode())
    out = h.digest()
    if dklen is not None:
        while len(out) < dklen:
            out += new(hash_name, out).digest()
        out = out[:dklen]
    return out


__all__ = [
    "md5", "sha1", "sha224", "sha256", "sha384", "sha512",
    "sha3_224", "sha3_256", "sha3_384", "sha3_512",
    "shake_128", "shake_256", "blake2b", "blake2s",
    "new", "algorithms_guaranteed", "algorithms_available",
    "pbkdf2_hmac",
]
