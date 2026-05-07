# pystro stdlib `uuid` (minimal — type 4 only).
import random


class UUID:
    def __init__(self, hex=None, int=None, version=4):
        if hex is not None:
            h = hex.replace("-", "").replace("{", "").replace("}", "")
            if len(h) != 32:
                raise ValueError("UUID: bad hex length")
            self.int = 0
            for c in h:
                v = int_lookup(c)
                self.int = (self.int << 4) | v
        elif int is not None:
            self.int = int
        self.version = version

    @property
    def hex(self):
        h = ""
        n = self.int
        for _ in range(32):
            h = "0123456789abcdef"[n & 0xf] + h
            n >>= 4
        return h

    def __str__(self):
        h = self.hex
        return h[0:8] + "-" + h[8:12] + "-" + h[12:16] + "-" + h[16:20] + "-" + h[20:32]

    def __repr__(self):
        return "UUID('" + str(self) + "')"

    def __eq__(self, o):
        return isinstance(o, UUID) and self.int == o.int

    def __hash__(self):
        return hash(self.int)


def int_lookup(c):
    if "0" <= c <= "9": return ord(c) - ord("0")
    if "a" <= c <= "f": return ord(c) - ord("a") + 10
    if "A" <= c <= "F": return ord(c) - ord("A") + 10
    raise ValueError("invalid hex char: " + c)


def uuid4():
    n = 0
    for _ in range(8):
        n = (n << 32) | random.randint(0, 0xffffffff)
    n = n & ((1 << 128) - 1)
    # Set version 4 (bits 12-15 of byte 6).
    n = (n & ~(0xf << 76)) | (0x4 << 76)
    # Set variant (top 2 bits of byte 8).
    n = (n & ~(0xc << 64)) | (0x8 << 64)
    return UUID(int=n, version=4)


NAMESPACE_DNS = UUID(int=0x6ba7b8109dad11d180b400c04fd430c8)
NAMESPACE_URL = UUID(int=0x6ba7b8119dad11d180b400c04fd430c8)
