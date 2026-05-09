"""pystro stub for `_random` (C accelerator behind random.Random)."""

import time as _time


class Random:
    """Mersenne-Twister-ish PRNG.  Pystro's seed handling is simpler
    than CPython's (no SHA-512 mixing); the goal is reproducibility +
    statistical adequacy for tests, not cryptographic strength."""

    def __init__(self, seed=None):
        self.seed(seed)

    def seed(self, a=None, version=2):
        if a is None:
            a = int(_time.time() * 1e6) ^ id(self)
        if isinstance(a, str):
            h = 0
            for ch in a:
                h = (h * 1315423911) ^ ord(ch)
            a = h
        elif isinstance(a, (bytes, bytearray)):
            h = 0
            for b in a:
                h = (h * 1315423911) ^ b
            a = h
        self._state = int(a) & 0xFFFFFFFFFFFFFFFF
        if self._state == 0:
            self._state = 0xDEADBEEFCAFEBABE
        return None

    def _next32(self):
        # xorshift64* — fast, decent quality, fully Python-implementable.
        s = self._state
        s ^= (s << 13) & 0xFFFFFFFFFFFFFFFF
        s ^= (s >> 7)
        s ^= (s << 17) & 0xFFFFFFFFFFFFFFFF
        self._state = s & 0xFFFFFFFFFFFFFFFF
        return ((s * 0x2545F4914F6CDD1D) >> 32) & 0xFFFFFFFF

    def random(self):
        # 53-bit float in [0, 1).
        a = self._next32() >> 5  # 27 bits
        b = self._next32() >> 6  # 26 bits
        return (a * 67108864.0 + b) / 9007199254740992.0

    def getrandbits(self, k):
        if k <= 0:
            raise ValueError("getrandbits: k must be positive")
        out = 0
        while k > 0:
            chunk = min(k, 32)
            out = (out << chunk) | (self._next32() >> (32 - chunk))
            k -= chunk
        return out

    def getstate(self):
        return (3, (self._state,), None)

    def setstate(self, state):
        if not isinstance(state, tuple) or state[0] != 3:
            raise ValueError("setstate: expected (3, internal, None)")
        self._state = state[1][0]
        return None


__all__ = ["Random"]
