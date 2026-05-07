# pystro stdlib `binascii` (subset).

_HEX = "0123456789abcdef"


def hexlify(data, sep=None):
    if isinstance(data, str):
        data = data.encode()
    data = bytes(data)
    out = []
    for i, b in enumerate(data):
        out.append(_HEX[(b >> 4) & 0xf])
        out.append(_HEX[b & 0xf])
        if sep and i + 1 < len(data):
            if isinstance(sep, bytes): sep = sep.decode()
            out.append(sep)
    return ("".join(out)).encode()


def b2a_hex(data, sep=None):
    return hexlify(data, sep)


def unhexlify(s):
    if isinstance(s, bytes):
        s = s.decode()
    if len(s) % 2 != 0:
        raise ValueError("odd-length hex")
    out = bytearray()
    for i in range(0, len(s), 2):
        a = s[i]; b = s[i+1]
        ai = "0123456789abcdef".find(a.lower())
        bi = "0123456789abcdef".find(b.lower())
        if ai < 0 or bi < 0:
            raise ValueError("invalid hex digit")
        out.append((ai << 4) | bi)
    return bytes(out)


def a2b_hex(s): return unhexlify(s)


def crc32(data, value=0):
    if isinstance(data, str):
        data = data.encode()
    data = bytes(data)
    crc = (~value) & 0xffffffff
    for b in data:
        crc = crc ^ b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xedb88320 if (crc & 1) else 0)
    return (~crc) & 0xffffffff


class Error(Exception):
    pass

class Incomplete(Exception):
    pass
