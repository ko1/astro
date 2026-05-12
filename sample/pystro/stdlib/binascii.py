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
    """UU-encode `data` (max 45 bytes) into one line ending with \n."""
    data = bytes(data)
    n = len(data)
    if n > 45:
        raise Error("At most 45 bytes at once")
    # Length-encoded byte: ' ' (or '`' with backtick) + n
    if n == 0 and backtick:
        return b"`\n"
    out = bytearray()
    out.append(0x60 + n if (backtick and n == 0) else 0x20 + n)
    # Pad to multiple of 3.
    pad = (-n) % 3
    buf = data + b"\x00" * pad
    for i in range(0, n + pad, 3):
        b0, b1, b2 = buf[i], buf[i+1], buf[i+2]
        c0 = (b0 >> 2) & 0x3F
        c1 = ((b0 << 4) | (b1 >> 4)) & 0x3F
        c2 = ((b1 << 2) | (b2 >> 6)) & 0x3F
        c3 = b2 & 0x3F
        for c in (c0, c1, c2, c3):
            if c == 0 and backtick:
                out.append(0x60)        # backtick instead of space
            else:
                out.append(0x20 + c if c != 0 else 0x60)
                # CPython actually maps 0 → space (0x20) unless backtick;
                # adjust:
    # Re-do simpler: rewrite mapping table.
    out = bytearray()
    out.append(0x20 + n if not backtick or n != 0 else 0x60)
    enc = lambda x: 0x60 if (backtick and x == 0) else 0x20 + x
    for i in range(0, n + pad, 3):
        b0, b1, b2 = buf[i], buf[i+1], buf[i+2]
        c0 = (b0 >> 2) & 0x3F
        c1 = ((b0 << 4) | (b1 >> 4)) & 0x3F
        c2 = ((b1 << 2) | (b2 >> 6)) & 0x3F
        c3 = b2 & 0x3F
        out.append(enc(c0))
        out.append(enc(c1))
        out.append(enc(c2))
        out.append(enc(c3))
    out.append(ord("\n"))
    return bytes(out)


def a2b_uu(s):
    """Decode a single line of UU-encoded data."""
    if isinstance(s, str):
        s = s.encode("ascii")
    s = bytes(s)
    if not s:
        return b""
    # Strip trailing newline(s).
    while s and s[-1:] in (b"\n", b"\r"):
        s = s[:-1]
    if not s:
        return b""
    # First char encodes length.
    n = (s[0] - 0x20) & 0x3F
    if n > 45:
        raise Error("Illegal char")
    body = s[1:]
    # Pad body if short.
    needed = ((n + 2) // 3) * 4
    if len(body) < needed:
        body = body + b" " * (needed - len(body))
    out = bytearray()
    for i in range(0, needed, 4):
        c0 = (body[i] - 0x20) & 0x3F
        c1 = (body[i+1] - 0x20) & 0x3F
        c2 = (body[i+2] - 0x20) & 0x3F
        c3 = (body[i+3] - 0x20) & 0x3F
        b0 = ((c0 << 2) | (c1 >> 4)) & 0xFF
        b1 = ((c1 << 4) | (c2 >> 2)) & 0xFF
        b2 = ((c2 << 6) | c3) & 0xFF
        out.append(b0)
        out.append(b1)
        out.append(b2)
    return bytes(out[:n])
def rledecode_hqx(data): return data
def rlecode_hqx(data):   return data
def crc_hqx(data, value): return value


__all__ = ["hexlify", "unhexlify", "b2a_hex", "a2b_hex",
           "crc32", "b2a_base64", "a2b_base64", "b2a_qp", "a2b_qp",
           "b2a_uu", "a2b_uu", "rledecode_hqx", "rlecode_hqx",
           "crc_hqx", "Error", "Incomplete"]
