# pystro stdlib `base64` — encode/decode standard Base64.

_TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"


def _table_index(ch):
    if "A" <= ch <= "Z":
        return ord(ch) - ord("A")
    if "a" <= ch <= "z":
        return ord(ch) - ord("a") + 26
    if "0" <= ch <= "9":
        return ord(ch) - ord("0") + 52
    if ch == "+": return 62
    if ch == "/": return 63
    return -1


def b64encode(data, altchars=None):
    if isinstance(data, str):
        data = data.encode()
    data = bytes(data)
    table = _TABLE
    if altchars is not None:
        if isinstance(altchars, bytes):
            altchars = altchars.decode()
        table = _TABLE[:62] + altchars[0] + altchars[1]
    out = []
    n = len(data)
    i = 0
    while i + 3 <= n:
        b0 = data[i]; b1 = data[i+1]; b2 = data[i+2]
        out.append(table[b0 >> 2])
        out.append(table[((b0 & 0x3) << 4) | (b1 >> 4)])
        out.append(table[((b1 & 0xf) << 2) | (b2 >> 6)])
        out.append(table[b2 & 0x3f])
        i += 3
    rem = n - i
    if rem == 1:
        b0 = data[i]
        out.append(table[b0 >> 2])
        out.append(table[(b0 & 0x3) << 4])
        out.append("=")
        out.append("=")
    elif rem == 2:
        b0 = data[i]; b1 = data[i+1]
        out.append(table[b0 >> 2])
        out.append(table[((b0 & 0x3) << 4) | (b1 >> 4)])
        out.append(table[(b1 & 0xf) << 2])
        out.append("=")
    return ("".join(out)).encode()


def b64decode(data, altchars=None, validate=False):
    if isinstance(data, bytes):
        data = data.decode()
    table_lookup = _table_index
    out = bytearray()
    cleaned = []
    for ch in data:
        if ch == "=":
            break
        if ch == "\n" or ch == "\r" or ch == " ":
            continue
        v = table_lookup(ch)
        if v < 0:
            if altchars and ch in altchars:
                # crude alt support
                if ch == altchars[0]: v = 62
                elif ch == altchars[1]: v = 63
                else:
                    if validate: raise ValueError("invalid base64")
                    continue
            else:
                if validate: raise ValueError("invalid base64")
                continue
        cleaned.append(v)
    n = len(cleaned)
    i = 0
    while i + 4 <= n:
        v0, v1, v2, v3 = cleaned[i], cleaned[i+1], cleaned[i+2], cleaned[i+3]
        out.append((v0 << 2) | (v1 >> 4))
        out.append(((v1 & 0xf) << 4) | (v2 >> 2))
        out.append(((v2 & 0x3) << 6) | v3)
        i += 4
    rem = n - i
    if rem == 2:
        v0, v1 = cleaned[i], cleaned[i+1]
        out.append((v0 << 2) | (v1 >> 4))
    elif rem == 3:
        v0, v1, v2 = cleaned[i], cleaned[i+1], cleaned[i+2]
        out.append((v0 << 2) | (v1 >> 4))
        out.append(((v1 & 0xf) << 4) | (v2 >> 2))
    return bytes(out)


def standard_b64encode(data): return b64encode(data)
def standard_b64decode(data): return b64decode(data)
def urlsafe_b64encode(data): return b64encode(data, altchars=b"-_")
def urlsafe_b64decode(data): return b64decode(data, altchars="-_")
def encodebytes(data):
    out = b64encode(data)
    return out + b"\n"
def decodebytes(data): return b64decode(data)
