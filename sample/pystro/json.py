# pystro stdlib `json` — minimal pure-Python implementation.
# Supports: dict, list, str, int, float, bool, None.
# Not supported: surrogate pairs, ASCII-only escapes, custom encoders.

def _esc_str(s):
    parts = ['"']
    for ch in s:
        c = ord(ch)
        if   ch == '"':  parts.append('\\"')
        elif ch == '\\': parts.append('\\\\')
        elif ch == '\n': parts.append('\\n')
        elif ch == '\t': parts.append('\\t')
        elif ch == '\r': parts.append('\\r')
        elif c < 0x20:
            parts.append("\\u%04x" % c)
        else:
            parts.append(ch)
    parts.append('"')
    return "".join(parts)

def _dump(v, indent=None, depth=0, sort_keys=False, default=None):
    if v is None:  return "null"
    if v is True:  return "true"
    if v is False: return "false"
    if isinstance(v, int):   return str(v)
    if isinstance(v, float):
        if v != v:  return "NaN"
        return repr(v)
    if isinstance(v, str):   return _esc_str(v)
    if isinstance(v, list) or isinstance(v, tuple):
        if not v: return "[]"
        if indent is None:
            return "[" + ", ".join([_dump(x, None, depth+1, sort_keys, default) for x in v]) + "]"
        sp = " " * (indent * (depth + 1))
        cl = " " * (indent * depth)
        parts = [_dump(x, indent, depth+1, sort_keys, default) for x in v]
        return "[\n" + ",\n".join(sp + p for p in parts) + "\n" + cl + "]"
    if isinstance(v, dict):
        if not v: return "{}"
        keys = list(v.keys()) if not sort_keys else sorted(v.keys())
        if indent is None:
            items = []
            for k in keys:
                if not isinstance(k, str):
                    raise TypeError("json: keys must be str")
                items.append(_esc_str(k) + ": " + _dump(v[k], None, depth+1, sort_keys, default))
            return "{" + ", ".join(items) + "}"
        sp = " " * (indent * (depth + 1))
        cl = " " * (indent * depth)
        items = []
        for k in keys:
            if not isinstance(k, str):
                raise TypeError("json: keys must be str")
            items.append(sp + _esc_str(k) + ": " + _dump(v[k], indent, depth+1, sort_keys, default))
        return "{\n" + ",\n".join(items) + "\n" + cl + "}"
    if default is not None:
        return _dump(default(v), indent, depth, sort_keys, default)
    raise TypeError("json: unsupported type")

def dumps(obj, indent=None, sort_keys=False, default=None, **kwargs):
    return _dump(obj, indent, 0, sort_keys, default)

def dump(obj, fp, indent=None, sort_keys=False, default=None, **kwargs):
    fp.write(dumps(obj, indent=indent, sort_keys=sort_keys, default=default))

def load(fp):
    return loads(fp.read())

# Parser: hand-rolled recursive-descent over a string with a single
# integer cursor (carried in a 1-element list to mimic by-ref).
class _P:
    def __init__(self, s):
        self.s = s
        self.i = 0
        self.n = len(s)

def _skip(p):
    while p.i < p.n and p.s[p.i] in " \t\n\r":
        p.i += 1

def _err(p, msg):
    raise ValueError("json: " + msg + " at " + str(p.i))

def _parse_str(p):
    _skip(p)
    if p.i >= p.n or p.s[p.i] != '"':
        _err(p, "expected string")
    p.i += 1
    out = []
    while p.i < p.n:
        ch = p.s[p.i]
        if ch == '"':
            p.i += 1
            return "".join(out)
        if ch == '\\':
            p.i += 1
            if p.i >= p.n: _err(p, "bad escape")
            e = p.s[p.i]; p.i += 1
            if   e == '"':  out.append('"')
            elif e == '\\': out.append('\\')
            elif e == '/':  out.append('/')
            elif e == 'n':  out.append('\n')
            elif e == 't':  out.append('\t')
            elif e == 'r':  out.append('\r')
            elif e == 'b':  out.append('\b')
            elif e == 'f':  out.append('\f')
            elif e == 'u':
                if p.i + 4 > p.n: _err(p, "bad \\u escape")
                hex_s = p.s[p.i:p.i+4]
                p.i += 4
                code = int(hex_s, 16)
                out.append(chr(code))
            else:
                _err(p, "bad escape")
        else:
            out.append(ch); p.i += 1
    _err(p, "unterminated string")

def _parse_num(p):
    start = p.i
    if p.s[p.i] == '-': p.i += 1
    while p.i < p.n and p.s[p.i] >= '0' and p.s[p.i] <= '9':
        p.i += 1
    is_float = False
    if p.i < p.n and p.s[p.i] == '.':
        is_float = True; p.i += 1
        while p.i < p.n and p.s[p.i] >= '0' and p.s[p.i] <= '9':
            p.i += 1
    if p.i < p.n and (p.s[p.i] == 'e' or p.s[p.i] == 'E'):
        is_float = True; p.i += 1
        if p.i < p.n and (p.s[p.i] == '+' or p.s[p.i] == '-'): p.i += 1
        while p.i < p.n and p.s[p.i] >= '0' and p.s[p.i] <= '9':
            p.i += 1
    num_s = p.s[start:p.i]
    if is_float:
        return float(num_s)
    return int(num_s)

def _parse_value(p):
    _skip(p)
    if p.i >= p.n: _err(p, "unexpected end")
    ch = p.s[p.i]
    if ch == '"':
        return _parse_str(p)
    if ch == '{':
        p.i += 1; _skip(p)
        d = {}
        if p.i < p.n and p.s[p.i] == '}':
            p.i += 1; return d
        while True:
            k = _parse_str(p)
            _skip(p)
            if p.i >= p.n or p.s[p.i] != ':': _err(p, "expected ':'")
            p.i += 1
            v = _parse_value(p)
            d[k] = v
            _skip(p)
            if p.i < p.n and p.s[p.i] == ',':
                p.i += 1; _skip(p); continue
            if p.i < p.n and p.s[p.i] == '}':
                p.i += 1; return d
            _err(p, "expected ',' or '}'")
    if ch == '[':
        p.i += 1; _skip(p)
        a = []
        if p.i < p.n and p.s[p.i] == ']':
            p.i += 1; return a
        while True:
            a.append(_parse_value(p))
            _skip(p)
            if p.i < p.n and p.s[p.i] == ',':
                p.i += 1; _skip(p); continue
            if p.i < p.n and p.s[p.i] == ']':
                p.i += 1; return a
            _err(p, "expected ',' or ']'")
    if ch == 't':
        if p.s[p.i:p.i+4] == "true":
            p.i += 4
            return True
    if ch == 'f':
        if p.s[p.i:p.i+5] == "false":
            p.i += 5
            return False
    if ch == 'n':
        if p.s[p.i:p.i+4] == "null":
            p.i += 4
            return None
    if ch == '-' or (ch >= '0' and ch <= '9'):
        return _parse_num(p)
    _err(p, "unexpected char")

def loads(s, object_hook=None):
    p = _P(s)
    p._object_hook = object_hook
    v = _parse_value(p)
    if object_hook is not None:
        v = _apply_hook(v, object_hook)
    _skip(p)
    if p.i != p.n:
        _err(p, "trailing data")
    return v


def _apply_hook(v, hook):
    if isinstance(v, dict):
        new = {}
        for k, val in v.items():
            new[k] = _apply_hook(val, hook)
        return hook(new)
    if isinstance(v, list):
        return [_apply_hook(x, hook) for x in v]
    return v

__all__ = ["dumps", "loads"]
