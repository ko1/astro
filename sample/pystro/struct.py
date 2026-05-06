# pystro stdlib `struct` (subset).
# Format chars: <>=! (byte order), b/B (i8/u8), h/H (i16/u16),
# i/I (i32/u32), q/Q (i64/u64), f/d (float/double), s (string).

def _is_big(fmt):
    if fmt and fmt[0] in "<>=!":
        return fmt[0] in (">", "!")
    return False

def _strip_endian(fmt):
    if fmt and fmt[0] in "<>=!":
        return fmt[1:]
    return fmt

def _size_of(ch):
    return {"b": 1, "B": 1, "h": 2, "H": 2, "i": 4, "I": 4, "l": 4, "L": 4,
            "q": 8, "Q": 8, "f": 4, "d": 8, "?": 1, "x": 1}.get(ch, 0)


def _pack_int(n, size, big, signed):
    if signed and n < 0:
        n += 1 << (8 * size)
    out = []
    for i in range(size):
        out.append(n & 0xff)
        n >>= 8
    if big:
        out = out[::-1]
    return bytes(out)


def _unpack_int(data, off, size, big, signed):
    n = 0
    if big:
        for i in range(size):
            n = (n << 8) | data[off + i]
    else:
        for i in range(size - 1, -1, -1):
            n = (n << 8) | data[off + i]
    if signed and (n & (1 << (8 * size - 1))):
        n -= 1 << (8 * size)
    return n


def calcsize(fmt):
    big = _is_big(fmt)
    fmt = _strip_endian(fmt)
    total = 0
    i = 0
    while i < len(fmt):
        ch = fmt[i]
        rep = 1
        if ch.isdigit():
            j = i
            while j < len(fmt) and fmt[j].isdigit():
                j += 1
            rep = int(fmt[i:j])
            i = j
            ch = fmt[i]
        if ch == "s":
            total += rep
        else:
            total += _size_of(ch) * rep
        i += 1
    return total


def pack(fmt, *args):
    big = _is_big(fmt)
    fmt = _strip_endian(fmt)
    out = bytearray()
    ai = 0
    i = 0
    while i < len(fmt):
        ch = fmt[i]
        rep = 1
        if ch.isdigit():
            j = i
            while j < len(fmt) and fmt[j].isdigit():
                j += 1
            rep = int(fmt[i:j])
            i = j
            ch = fmt[i]
        if ch == "s":
            v = args[ai]; ai += 1
            if isinstance(v, str): v = v.encode()
            v = bytes(v)
            if len(v) < rep:
                v = v + b"\x00" * (rep - len(v))
            out += v[:rep]
        elif ch in "bBhHiIlLqQ":
            sz = _size_of(ch)
            signed = ch in "bhilq"
            for _ in range(rep):
                out += _pack_int(args[ai], sz, big, signed); ai += 1
        elif ch == "?":
            for _ in range(rep):
                out.append(1 if args[ai] else 0); ai += 1
        elif ch == "x":
            for _ in range(rep):
                out.append(0)
        elif ch == "f" or ch == "d":
            for _ in range(rep):
                v = float(args[ai]); ai += 1
                if ch == "f":
                    bits = __pystro_float_to_bits__(v, False) & 0xffffffff
                    out += _pack_int(bits, 4, big, False)
                else:
                    bits = __pystro_float_to_bits__(v, True) & 0xffffffffffffffff
                    out += _pack_int(bits, 8, big, False)
        i += 1
    return bytes(out)


def unpack(fmt, data):
    big = _is_big(fmt)
    fmt = _strip_endian(fmt)
    out = []
    off = 0
    data = bytes(data)
    i = 0
    while i < len(fmt):
        ch = fmt[i]
        rep = 1
        if ch.isdigit():
            j = i
            while j < len(fmt) and fmt[j].isdigit():
                j += 1
            rep = int(fmt[i:j])
            i = j
            ch = fmt[i]
        if ch == "s":
            out.append(data[off:off + rep])
            off += rep
        elif ch in "bBhHiIlLqQ":
            sz = _size_of(ch)
            signed = ch in "bhilq"
            for _ in range(rep):
                out.append(_unpack_int(data, off, sz, big, signed))
                off += sz
        elif ch == "?":
            for _ in range(rep):
                out.append(data[off] != 0); off += 1
        elif ch == "x":
            off += rep
        elif ch == "f":
            for _ in range(rep):
                bits = _unpack_int(data, off, 4, big, False)
                out.append(__pystro_bits_to_float__(bits, False))
                off += 4
        elif ch == "d":
            for _ in range(rep):
                bits = _unpack_int(data, off, 8, big, False)
                out.append(__pystro_bits_to_float__(bits, True))
                off += 8
        i += 1
    return tuple(out)


def pack_into(fmt, buffer, offset, *args):
    p = pack(fmt, *args)
    for i, b in enumerate(p):
        buffer[offset + i] = b


def unpack_from(fmt, data, offset=0):
    sz = calcsize(fmt)
    return unpack(fmt, data[offset:offset + sz])


class error(Exception):
    pass
