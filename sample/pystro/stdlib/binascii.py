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


class Error(ValueError):
    pass

class Incomplete(Exception):
    pass


# RFC 3548 base64 — pure-Python encoder/decoder.
_B64_ALPHA = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
_B64_REV = {c: i for i, c in enumerate(_B64_ALPHA)}


def b2a_base64(data, *, newline=True):
    if isinstance(data, str): data = data.encode()
    data = bytes(data)
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        chunk = data[i:i+3]
        i += 3
        b = chunk + b"\x00" * (3 - len(chunk))
        v = (b[0] << 16) | (b[1] << 8) | b[2]
        out.append(_B64_ALPHA[(v >> 18) & 0x3F])
        out.append(_B64_ALPHA[(v >> 12) & 0x3F])
        if len(chunk) >= 2:
            out.append(_B64_ALPHA[(v >> 6) & 0x3F])
        else:
            out.append(ord("="))
        if len(chunk) >= 3:
            out.append(_B64_ALPHA[v & 0x3F])
        else:
            out.append(ord("="))
    if newline: out.append(ord("\n"))
    return bytes(out)


def a2b_base64(s, *, strict_mode=False):
    if isinstance(s, str): s = s.encode("ascii")
    s = bytes(s)
    out = bytearray()
    buf = 0
    bits = 0
    for ch in s:
        if ch in (ord(" "), ord("\n"), ord("\t"), ord("\r")):
            continue
        if ch == ord("="):
            break
        v = _B64_REV.get(ch)
        if v is None:
            if strict_mode: raise Error("Invalid base64")
            continue
        buf = (buf << 6) | v
        bits += 6
        if bits >= 8:
            bits -= 8
            out.append((buf >> bits) & 0xFF)
    return bytes(out)


def b2a_qp(data, quotetabs=False, istext=True, header=False):
    if isinstance(data, str): data = data.encode()
    out = bytearray()
    for ch in data:
        if 33 <= ch <= 126 and ch != ord("="):
            out.append(ch)
        elif ch == ord(" ") and not quotetabs:
            out.append(ch)
        else:
            out.extend(b"=%02X" % ch)
    return bytes(out)


def a2b_qp(s, header=False):
    if isinstance(s, str): s = s.encode()
    out = bytearray()
    i = 0
    while i < len(s):
        ch = s[i]
        if ch == ord("=") and i + 2 < len(s):
            try:
                out.append(int(s[i+1:i+3], 16))
                i += 3
                continue
            except ValueError:
                pass
        out.append(ch)
        i += 1
    return bytes(out)


def b2a_uu(data, *, backtick=False):
    raise NotImplementedError("b2a_uu")
def a2b_uu(s):
    raise NotImplementedError("a2b_uu")
def rledecode_hqx(data): return data
def rlecode_hqx(data):   return data
def crc_hqx(data, value): return value


__all__ = ["hexlify", "unhexlify", "b2a_hex", "a2b_hex",
           "crc32", "b2a_base64", "a2b_base64", "b2a_qp", "a2b_qp",
           "b2a_uu", "a2b_uu", "rledecode_hqx", "rlecode_hqx",
           "crc_hqx", "Error", "Incomplete"]
