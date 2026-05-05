# pystro stdlib `pickle` (minimal, custom text-based serialization).
#
# Not bytecode-compatible with CPython's pickle.  Designed only to
# round-trip simple values: int/float/str/bool/None/list/tuple/dict/set.
# Uses a tag-prefixed text format.

import io as _io


def _dumps_inner(v, out):
    if v is None:
        out.append("N")
    elif v is True:
        out.append("T")
    elif v is False:
        out.append("F")
    elif isinstance(v, int):
        out.append("i")
        out.append(str(v))
        out.append(";")
    elif isinstance(v, float):
        out.append("f")
        out.append(repr(v))
        out.append(";")
    elif isinstance(v, str):
        out.append("s")
        out.append(str(len(v)))
        out.append(":")
        out.append(v)
    elif isinstance(v, list):
        out.append("[")
        out.append(str(len(v)))
        out.append(";")
        for x in v:
            _dumps_inner(x, out)
    elif isinstance(v, tuple):
        out.append("(")
        out.append(str(len(v)))
        out.append(";")
        for x in v:
            _dumps_inner(x, out)
    elif isinstance(v, dict):
        out.append("{")
        out.append(str(len(v)))
        out.append(";")
        for k in v:
            _dumps_inner(k, out)
            _dumps_inner(v[k], out)
    elif isinstance(v, set):
        out.append("S")
        items = list(v)
        out.append(str(len(items)))
        out.append(";")
        for x in items:
            _dumps_inner(x, out)
    else:
        raise TypeError("pickle: cannot serialize " + str(type(v)))


def dumps(v):
    out = []
    _dumps_inner(v, out)
    return "".join(out).encode()


# Loader uses an integer cursor (carried in [pos] one-element list).
def _read_until(buf, pos, ch):
    start = pos[0]
    while pos[0] < len(buf) and buf[pos[0]] != ch:
        pos[0] += 1
    s = buf[start:pos[0]]
    pos[0] += 1   # past ch
    return s


def _loads_inner(buf, pos):
    if pos[0] >= len(buf):
        raise ValueError("pickle: truncated")
    tag = buf[pos[0]]
    pos[0] += 1
    if tag == "N": return None
    if tag == "T": return True
    if tag == "F": return False
    if tag == "i":
        return int(_read_until(buf, pos, ";"))
    if tag == "f":
        return float(_read_until(buf, pos, ";"))
    if tag == "s":
        L = int(_read_until(buf, pos, ":"))
        s = buf[pos[0]:pos[0]+L]
        pos[0] += L
        return s
    if tag == "[":
        L = int(_read_until(buf, pos, ";"))
        return [_loads_inner(buf, pos) for _ in range(L)]
    if tag == "(":
        L = int(_read_until(buf, pos, ";"))
        return tuple(_loads_inner(buf, pos) for _ in range(L))
    if tag == "{":
        L = int(_read_until(buf, pos, ";"))
        d = {}
        for _ in range(L):
            k = _loads_inner(buf, pos)
            v = _loads_inner(buf, pos)
            d[k] = v
        return d
    if tag == "S":
        L = int(_read_until(buf, pos, ";"))
        s = set()
        for _ in range(L):
            s.add(_loads_inner(buf, pos))
        return s
    raise ValueError("pickle: unknown tag " + repr(tag))


def loads(b):
    if isinstance(b, bytes):
        # Decode bytes back to chars (pystro bytes is byte-array of chars).
        s = ""
        for byte in b:
            s = s + chr(byte)
    else:
        s = b
    pos = [0]
    return _loads_inner(s, pos)


def dump(obj, fp):
    fp.write(dumps(obj))


def load(fp):
    return loads(fp.read())


__all__ = ["dumps", "loads", "dump", "load"]
