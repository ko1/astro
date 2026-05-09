"""pystro stub for `_struct` (CPython's binary-packing C accelerator).

Implements the format codes needed by typical CPython stdlib clients:
    b B h H i I l L q Q f d s c x p ?    (with byte-order prefix < > ! = @)

This is a pure-Python implementation; precision and edge-case handling
match CPython for everyday use, but exotic format strings (native
alignment beyond what's listed, padding tricks) may differ.
"""

import sys as _sys


__doc__ = "pystro _struct stub"


class error(Exception):
    pass


_NATIVE_LITTLE = (_sys.byteorder == "little")


def _parse_fmt(fmt):
    """Yield (count, code) pairs along with the byteorder. Honours leading
    `<`, `>`, `!`, `=`, `@` prefix (default native)."""
    if not isinstance(fmt, str):
        if isinstance(fmt, (bytes, bytearray)):
            fmt = fmt.decode("ascii")
        else:
            raise error("Struct() argument 1 must be a str or bytes object")
    little = _NATIVE_LITTLE
    i = 0
    if fmt and fmt[0] in "@=<>!":
        c = fmt[0]
        if c == "<": little = True
        elif c in ">!": little = False
        elif c == "=": little = _NATIVE_LITTLE
        i = 1
    items = []
    while i < len(fmt):
        c = fmt[i]
        if c == " ":
            i += 1
            continue
        # Optional leading count.
        cnt = 0
        has_cnt = False
        while i < len(fmt) and fmt[i].isdigit():
            cnt = cnt * 10 + int(fmt[i])
            has_cnt = True
            i += 1
        if i >= len(fmt):
            raise error("repeat count given without format specifier")
        code = fmt[i]
        i += 1
        if not has_cnt:
            cnt = 1
        items.append((cnt, code))
    return little, items


_SIZES = {
    "b": 1, "B": 1, "?": 1, "c": 1, "x": 1,
    "h": 2, "H": 2, "e": 2,
    "i": 4, "I": 4, "l": 4, "L": 4, "f": 4,
    "q": 8, "Q": 8, "d": 8, "n": 8, "N": 8,
}


def calcsize(fmt):
    little, items = _parse_fmt(fmt)
    sz = 0
    for cnt, code in items:
        if code in ("s", "p"):
            sz += cnt
        elif code in _SIZES:
            sz += _SIZES[code] * cnt
        else:
            raise error(f"bad char in struct format: {code!r}")
    return sz


def _to_signed(n, bits):
    half = 1 << (bits - 1)
    if n >= half:
        n -= (1 << bits)
    return n


def _from_signed(n, bits):
    if n < 0:
        n += (1 << bits)
    return n


def _pack_int(value, size, little, signed):
    if not isinstance(value, int):
        raise error("required argument is not an integer")
    if signed:
        lo = -(1 << (size * 8 - 1))
        hi = (1 << (size * 8 - 1)) - 1
        if not (lo <= value <= hi):
            raise error("argument out of range")
        if value < 0:
            value += (1 << (size * 8))
    else:
        if not (0 <= value < (1 << (size * 8))):
            raise error("argument out of range")
    out = bytearray(size)
    for k in range(size):
        out[k] = (value >> (8 * k)) & 0xFF
    if not little:
        out.reverse()
    return bytes(out)


def _unpack_int(buf, size, little, signed):
    n = 0
    if little:
        for k in range(size):
            n |= buf[k] << (8 * k)
    else:
        for k in range(size):
            n = (n << 8) | buf[k]
    if signed:
        return _to_signed(n, size * 8)
    return n


def _pack_float(value, size, little):
    # Best-effort: Python doesn't expose IEEE conversion natively,
    # but we can use struct round-trip via int representation.
    import math
    if size == 8:
        if math.isnan(value):
            bits = 0x7FF8000000000000
        elif math.isinf(value):
            bits = 0x7FF0000000000000 if value > 0 else 0xFFF0000000000000
        elif value == 0:
            bits = 0x8000000000000000 if math.copysign(1, value) < 0 else 0
        else:
            sign = 0 if value > 0 else 1
            value = abs(value)
            m, e = math.frexp(value)
            m = m * 2 - 1
            e -= 1
            mantissa = int(m * (1 << 52))
            biased = e + 1023
            bits = (sign << 63) | ((biased & 0x7FF) << 52) | (mantissa & ((1 << 52) - 1))
        return _pack_int(bits, 8, little, False)
    elif size == 4:
        # Single precision via doubles.
        if math.isnan(value):
            bits = 0x7FC00000
        elif math.isinf(value):
            bits = 0x7F800000 if value > 0 else 0xFF800000
        elif value == 0:
            bits = 0x80000000 if math.copysign(1, value) < 0 else 0
        else:
            sign = 0 if value > 0 else 1
            value = abs(value)
            m, e = math.frexp(value)
            m = m * 2 - 1
            e -= 1
            mantissa = int(m * (1 << 23))
            biased = e + 127
            bits = (sign << 31) | ((biased & 0xFF) << 23) | (mantissa & ((1 << 23) - 1))
        return _pack_int(bits, 4, little, False)
    raise error(f"bad float size {size}")


def _unpack_float(buf, size, little):
    bits = _unpack_int(buf, size, little, False)
    if size == 8:
        sign = (bits >> 63) & 1
        e = (bits >> 52) & 0x7FF
        m = bits & ((1 << 52) - 1)
        if e == 0x7FF:
            if m: return float("nan")
            return float("-inf") if sign else float("inf")
        if e == 0:
            value = m / (1 << 52) * (2 ** -1022)
        else:
            value = (1 + m / (1 << 52)) * (2 ** (e - 1023))
        return -value if sign else value
    elif size == 4:
        sign = (bits >> 31) & 1
        e = (bits >> 23) & 0xFF
        m = bits & ((1 << 23) - 1)
        if e == 0xFF:
            if m: return float("nan")
            return float("-inf") if sign else float("inf")
        if e == 0:
            value = m / (1 << 23) * (2 ** -126)
        else:
            value = (1 + m / (1 << 23)) * (2 ** (e - 127))
        return -value if sign else value
    raise error(f"bad float size {size}")


def pack(fmt, *args):
    little, items = _parse_fmt(fmt)
    out = bytearray()
    ai = 0
    for cnt, code in items:
        if code == "x":
            out.extend(b"\x00" * cnt)
            continue
        if code == "s":
            v = args[ai]; ai += 1
            if isinstance(v, str): v = v.encode("latin-1")
            buf = bytes(v[:cnt]).ljust(cnt, b"\x00")
            out.extend(buf)
            continue
        if code == "p":
            v = args[ai]; ai += 1
            if isinstance(v, str): v = v.encode("latin-1")
            n = min(len(v), cnt - 1, 255)
            out.append(n)
            out.extend(v[:n])
            out.extend(b"\x00" * (cnt - 1 - n))
            continue
        for _ in range(cnt):
            v = args[ai]; ai += 1
            if code in ("b", "B", "h", "H", "i", "I", "l", "L", "q", "Q", "n", "N"):
                signed = code in ("b", "h", "i", "l", "q", "n")
                size = _SIZES[code]
                out.extend(_pack_int(int(v), size, little, signed))
            elif code in ("f", "d", "e"):
                size = _SIZES[code] if code != "e" else 2
                if size == 2: raise error("half-precision not supported")
                out.extend(_pack_float(float(v), size, little))
            elif code == "?":
                out.append(1 if v else 0)
            elif code == "c":
                if isinstance(v, str): v = v.encode("latin-1")
                if len(v) != 1: raise error("char format requires 1-byte input")
                out.extend(v)
            else:
                raise error(f"bad char in struct format: {code!r}")
    return bytes(out)


def unpack(fmt, buf):
    return tuple(_iter_unpack_one(fmt, buf, 0))


def unpack_from(fmt, buffer, offset=0):
    return tuple(_iter_unpack_one(fmt, buffer, offset))


def _iter_unpack_one(fmt, buffer, offset):
    little, items = _parse_fmt(fmt)
    out = []
    pos = offset
    for cnt, code in items:
        if code == "x":
            pos += cnt
            continue
        if code == "s":
            out.append(bytes(buffer[pos:pos + cnt]))
            pos += cnt
            continue
        if code == "p":
            n = buffer[pos]
            out.append(bytes(buffer[pos + 1:pos + 1 + min(n, cnt - 1)]))
            pos += cnt
            continue
        for _ in range(cnt):
            if code in ("b", "B", "h", "H", "i", "I", "l", "L", "q", "Q", "n", "N"):
                signed = code in ("b", "h", "i", "l", "q", "n")
                size = _SIZES[code]
                out.append(_unpack_int(bytes(buffer[pos:pos + size]), size, little, signed))
                pos += size
            elif code in ("f", "d"):
                size = _SIZES[code]
                out.append(_unpack_float(bytes(buffer[pos:pos + size]), size, little))
                pos += size
            elif code == "?":
                out.append(buffer[pos] != 0)
                pos += 1
            elif code == "c":
                out.append(bytes(buffer[pos:pos + 1]))
                pos += 1
            else:
                raise error(f"bad char in struct format: {code!r}")
    return out


def iter_unpack(fmt, buffer):
    sz = calcsize(fmt)
    for i in range(0, len(buffer), sz):
        yield tuple(_iter_unpack_one(fmt, buffer, i))


def pack_into(fmt, buffer, offset, *args):
    data = pack(fmt, *args)
    if hasattr(buffer, "__setitem__"):
        for i, b in enumerate(data):
            buffer[offset + i] = b
    return None


class Struct:
    def __init__(self, fmt):
        self.format = fmt
        self.size = calcsize(fmt)
    def pack(self, *args):
        return pack(self.format, *args)
    def unpack(self, buf):
        return unpack(self.format, buf)
    def unpack_from(self, buf, offset=0):
        return unpack_from(self.format, buf, offset)
    def iter_unpack(self, buf):
        return iter_unpack(self.format, buf)
    def pack_into(self, buf, offset, *args):
        return pack_into(self.format, buf, offset, *args)


def _clearcache():
    pass


__all__ = ["calcsize", "pack", "unpack", "pack_into", "unpack_from",
           "iter_unpack", "Struct", "error", "_clearcache", "__doc__"]
