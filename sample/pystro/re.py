# pystro stdlib `re` — VERY minimal stub.
#
# Real regex support is deferred to `sample/astrorge` integration.
# This stub handles only literal strings and `.` / `*` / `+` / `?` /
# character classes / `^` / `$` — enough for many simple uses.
#
# Real implementations should be migrated when astrorge lands.

IGNORECASE = 2
I = IGNORECASE
MULTILINE = 8
M = MULTILINE
DOTALL = 16
S = DOTALL
VERBOSE = 64
X = VERBOSE
ASCII = 256
A = ASCII

class error(Exception):
    pass


def _match_here(pat, pi, s, si, flags):
    """Match `pat[pi:]` against `s[si:]`.  Returns (success, end_index) or (False, -1).
    Greedy matching for *, +, ?."""
    if pi >= len(pat):
        return True, si
    # Anchor $.
    if pat[pi] == "$" and pi + 1 == len(pat):
        return (si == len(s)), si
    # Find the position of the quantifier (if any) — it lives just past
    # the current atom (which may span multiple chars: `\d`, `[...]`).
    after = _advance(pat, pi)
    nxt = pat[after] if after < len(pat) else ""
    if nxt == "*":
        return _match_star(pat, pi, after, s, si, flags)
    if nxt == "+":
        ok, ei = _match_one(pat, pi, s, si, flags)
        if not ok: return False, -1
        return _match_star(pat, pi, after, s, ei, flags)
    if nxt == "?":
        ok, ei = _match_one(pat, pi, s, si, flags)
        if ok:
            ok2, ei2 = _match_here(pat, after + 1, s, ei, flags)
            if ok2: return True, ei2
        return _match_here(pat, after + 1, s, si, flags)
    # Single match.
    ok, ei = _match_one(pat, pi, s, si, flags)
    if not ok: return False, -1
    return _match_here(pat, after, s, ei, flags)


def _advance(pat, pi):
    if pat[pi] == "\\" and pi + 1 < len(pat):
        return pi + 2
    if pat[pi] == "[":
        end = pat.find("]", pi + 1)
        return end + 1 if end >= 0 else len(pat)
    return pi + 1


def _match_one(pat, pi, s, si, flags):
    if si >= len(s): return False, -1
    ch = s[si]
    pc = pat[pi]
    if pc == "\\" and pi + 1 < len(pat):
        esc = pat[pi + 1]
        if esc == "d": return (ch.isdigit(), si + 1)
        if esc == "D": return (not ch.isdigit(), si + 1)
        if esc == "w": return (ch.isalnum() or ch == "_", si + 1)
        if esc == "W": return (not (ch.isalnum() or ch == "_"), si + 1)
        if esc == "s": return (ch in " \t\n\r\f\v", si + 1)
        if esc == "S": return (ch not in " \t\n\r\f\v", si + 1)
        if esc == "n": return (ch == "\n", si + 1)
        if esc == "t": return (ch == "\t", si + 1)
        return (ch == esc, si + 1)
    if pc == ".":
        return (True, si + 1)
    if pc == "[":
        end = pat.find("]", pi + 1)
        if end < 0: return False, -1
        cls = pat[pi + 1:end]
        neg = False
        if cls.startswith("^"):
            neg = True
            cls = cls[1:]
        matched = False
        i = 0
        while i < len(cls):
            if i + 2 < len(cls) and cls[i + 1] == "-":
                if cls[i] <= ch <= cls[i + 2]:
                    matched = True
                i += 3
            else:
                if cls[i] == ch:
                    matched = True
                i += 1
        return ((matched != neg), si + 1)
    if flags & IGNORECASE:
        if ch.lower() == pc.lower(): return True, si + 1
    if ch == pc: return True, si + 1
    return False, -1


def _match_star(pat, atom_pi, after_quant_pi, s, si, flags):
    # Greedy: consume as many as possible, then backtrack.
    # `atom_pi` is where the atom starts; `after_quant_pi` is the
    # position past the * / + / ? quantifier (one beyond `_advance`).
    matches = [si]
    while True:
        ok, ei = _match_one(pat, atom_pi, s, matches[-1], flags)
        if not ok: break
        matches.append(ei)
    while matches:
        cur = matches.pop()
        ok, ei = _match_here(pat, after_quant_pi + 1, s, cur, flags)
        if ok: return True, ei
    return False, -1


class Match:
    def __init__(self, s, start, end, groups=None):
        self._s = s
        self._start = start
        self._end = end
        self._groups = groups or []
    def group(self, n=0):
        if n == 0: return self._s[self._start:self._end]
        return self._groups[n - 1] if n - 1 < len(self._groups) else ""
    def groups(self):
        return tuple(self._groups)
    def start(self, n=0):
        return self._start
    def end(self, n=0):
        return self._end
    def span(self, n=0):
        return (self._start, self._end)
    def __bool__(self):
        return True


class Pattern:
    def __init__(self, pat, flags=0):
        self.pattern = pat
        self.flags = flags
    def match(self, s, pos=0, endpos=None):
        pat = self.pattern
        anchored = pat.startswith("^")
        if anchored: pat_body = pat[1:]
        else:        pat_body = pat
        ok, ei = _match_here(pat_body, 0, s, pos, self.flags)
        if ok: return Match(s, pos, ei)
        return None
    def fullmatch(self, s, pos=0, endpos=None):
        m = self.match(s, pos)
        if m and m._end == len(s): return m
        return None
    def search(self, s, pos=0, endpos=None):
        pat = self.pattern
        anchored = pat.startswith("^")
        pat_body = pat[1:] if anchored else pat
        if anchored:
            return self.match(s, pos)
        for i in range(pos, len(s) + 1):
            ok, ei = _match_here(pat_body, 0, s, i, self.flags)
            if ok:
                return Match(s, i, ei)
        return None
    def findall(self, s, pos=0, endpos=None):
        out = []
        i = pos
        while i <= len(s):
            m = self.search(s, i)
            if not m: break
            out.append(m.group())
            i = m._end if m._end > i else i + 1
        return out
    def finditer(self, s, pos=0, endpos=None):
        out = self.findall(s, pos, endpos)
        for x in out:
            # Re-search to get Match objects.
            m = self.search(s, pos)
            if m:
                yield m
                pos = m._end + (1 if m._end == m._start else 0)
    def sub(self, repl, s, count=0):
        out = []
        i = 0
        n = 0
        while i <= len(s):
            if count and n >= count:
                out.append(s[i:])
                break
            m = self.search(s, i)
            if not m:
                out.append(s[i:])
                break
            out.append(s[i:m._start])
            if callable(repl):
                out.append(repl(m))
            else:
                out.append(repl)
            i = m._end
            if m._end == m._start: i += 1
            n += 1
        return "".join(out)
    def split(self, s, maxsplit=0):
        out = []
        i = 0
        n = 0
        while i <= len(s):
            if maxsplit and n >= maxsplit:
                out.append(s[i:]); break
            m = self.search(s, i)
            if not m:
                out.append(s[i:]); break
            out.append(s[i:m._start])
            i = m._end
            if m._end == m._start: i += 1
            n += 1
        return out


def compile(pattern, flags=0):
    return Pattern(pattern, flags)


def match(pattern, s, flags=0):
    return compile(pattern, flags).match(s)

def fullmatch(pattern, s, flags=0):
    return compile(pattern, flags).fullmatch(s)

def search(pattern, s, flags=0):
    return compile(pattern, flags).search(s)

def findall(pattern, s, flags=0):
    return compile(pattern, flags).findall(s)

def finditer(pattern, s, flags=0):
    return compile(pattern, flags).finditer(s)

def sub(pattern, repl, s, count=0, flags=0):
    return compile(pattern, flags).sub(repl, s, count)

def split(pattern, s, maxsplit=0, flags=0):
    return compile(pattern, flags).split(s, maxsplit)


def escape(s):
    out = []
    for ch in s:
        if ch in r".^$*+?()[]{}|\\":
            out.append("\\")
        out.append(ch)
    return "".join(out)
