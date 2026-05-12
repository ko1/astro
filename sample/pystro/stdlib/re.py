# pystro stdlib `re` — minimal but covers the common cases:
#   literals, '.', '*', '+', '?', character classes, '^' / '$',
#   character escapes (\d \w \s \D \W \S), groups '(...)' (no
#   non-capturing or lookaround).
#
# Real regex support is deferred to `sample/astrorge` integration.

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
UNICODE = 32
U = UNICODE
LOCALE = 4
L = LOCALE
DEBUG = 128
TEMPLATE = 1
T = TEMPLATE
NOFLAG = 0


class error(Exception):
    pass


class Scanner:
    """Pystro stub for re.Scanner.  Stores patterns but never actually
    tokenizes — CPython tests that import this for type checks at least
    don't AttributeError."""
    def __init__(self, lexicon, flags=0):
        self.lexicon = lexicon
        self.flags = flags
    def scan(self, string):
        return ([], string)


def _find_group_close(pat, pi):
    # `pat[pi]` is '('.  Return index of matching ')'.
    depth = 1
    i = pi + 1
    while i < len(pat):
        if pat[i] == "\\":
            i += 2
            continue
        if pat[i] == "(":
            depth += 1
        elif pat[i] == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _advance(pat, pi):
    if pat[pi] == "\\" and pi + 1 < len(pat):
        return pi + 2
    if pat[pi] == "[":
        end = pat.find("]", pi + 1)
        return end + 1 if end >= 0 else len(pat)
    if pat[pi] == "(":
        end = _find_group_close(pat, pi)
        return end + 1 if end >= 0 else len(pat)
    return pi + 1


def _match_one(pat, pi, s, si, flags, groups):
    """Match a single 'atom' starting at pat[pi] against s[si:].
    Returns (ok, end_index)."""
    if pat[pi] == "(":
        end = _find_group_close(pat, pi)
        if end < 0:
            return False, -1
        # Detect non-capturing (?:...) — start group_idx assignment after.
        body = pat[pi + 1:end]
        is_capt = not body.startswith("?:")
        if not is_capt:
            body = body[2:]
        if is_capt:
            grp_idx = len(groups)
            groups.append(None)
        ok, ei = _match_here(body, 0, s, si, flags, groups)
        if not ok:
            if is_capt:
                groups.pop()
            return False, -1
        if is_capt:
            groups[grp_idx] = s[si:ei]
        return True, ei
    if si >= len(s):
        return False, -1
    ch = s[si]
    pc = pat[pi]
    if pc == "\\" and pi + 1 < len(pat):
        esc = pat[pi + 1]
        if esc == "d": return (ch.isdigit(), si + 1)
        if esc == "D": return (not ch.isdigit(), si + 1)
        if esc == "w": return ((ch.isalnum() or ch == "_"), si + 1)
        if esc == "W": return ((not (ch.isalnum() or ch == "_")), si + 1)
        if esc == "s": return ((ch in " \t\n\r\f\v"), si + 1)
        if esc == "S": return ((ch not in " \t\n\r\f\v"), si + 1)
        if esc == "n": return (ch == "\n", si + 1)
        if esc == "t": return (ch == "\t", si + 1)
        if esc == "b": return (False, -1)  # word-boundary not supported
        # Otherwise: literal escape (e.g. \., \\, \(, \) etc.)
        if (flags & IGNORECASE) and ch.lower() == esc.lower():
            return True, si + 1
        return (ch == esc, si + 1)
    if pc == ".":
        # By default, '.' matches anything except newline.
        if (flags & DOTALL) or ch != "\n":
            return True, si + 1
        return False, -1
    if pc == "[":
        end = pat.find("]", pi + 1)
        if end < 0:
            return False, -1
        cls = pat[pi + 1:end]
        neg = False
        if cls.startswith("^"):
            neg = True
            cls = cls[1:]
        matched = False
        i = 0
        cl = ch.lower() if (flags & IGNORECASE) else ch
        while i < len(cls):
            if cls[i] == "\\" and i + 1 < len(cls):
                # \d \w \s etc inside char class
                e = cls[i + 1]
                if e == "d" and ch.isdigit(): matched = True
                elif e == "w" and (ch.isalnum() or ch == "_"): matched = True
                elif e == "s" and ch in " \t\n\r\f\v": matched = True
                elif e == "n" and ch == "\n": matched = True
                elif e == "t" and ch == "\t": matched = True
                elif e == ch: matched = True
                i += 2
                continue
            if i + 2 < len(cls) and cls[i + 1] == "-":
                lo = cls[i]; hi = cls[i + 2]
                if (flags & IGNORECASE):
                    lo = lo.lower(); hi = hi.lower()
                if lo <= cl <= hi:
                    matched = True
                i += 3
            else:
                cc = cls[i].lower() if (flags & IGNORECASE) else cls[i]
                if cc == cl:
                    matched = True
                i += 1
        return ((matched != neg), si + 1)
    if flags & IGNORECASE:
        if ch.lower() == pc.lower():
            return True, si + 1
    if ch == pc:
        return True, si + 1
    return False, -1


def _match_star(pat, atom_pi, after_quant_pi, s, si, flags, groups, lazy=False):
    # Greedy by default: match as many as possible, then backtrack.
    saved_groups = list(groups)
    matches = [(si, list(groups))]
    while True:
        # Reset groups to before this attempted match.
        del groups[:]
        groups.extend(matches[-1][1])
        ok, ei = _match_one(pat, atom_pi, s, matches[-1][0], flags, groups)
        if not ok or ei == matches[-1][0]:
            break
        matches.append((ei, list(groups)))
    if lazy: matches = matches  # for symmetry, then iterate left-to-right
    order = list(reversed(matches)) if not lazy else matches
    for cur_si, cur_grps in order:
        del groups[:]
        groups.extend(cur_grps)
        ok, ei = _match_here(pat, after_quant_pi, s, cur_si, flags, groups)
        if ok:
            return True, ei
    del groups[:]
    groups.extend(saved_groups)
    return False, -1


def _find_top_alt(pat, pi):
    """Scan for top-level `|` at or after pi.  Returns the index of the
    first such `|`, or -1 if none.  `[`...`]` and `(`...`)` and escapes
    are skipped."""
    depth = 0
    i = pi
    while i < len(pat):
        c = pat[i]
        if c == "\\" and i + 1 < len(pat):
            i += 2
            continue
        if c == "[":
            end = pat.find("]", i + 1)
            i = (end + 1) if end >= 0 else len(pat)
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == "|" and depth == 0:
            return i
        i += 1
    return -1


def _match_here(pat, pi, s, si, flags, groups):
    # Top-level alternation: try left, then right.  Each branch is
    # `pat[pi:alt]` (left) and `pat[alt+1:]` (right).
    alt = _find_top_alt(pat, pi)
    if alt >= 0:
        saved = list(groups)
        # Left branch — match left part as a complete sub-pattern.
        left = pat[pi:alt]
        ok, ei = _match_here(left, 0, s, si, flags, groups)
        if ok:
            return True, ei
        del groups[:]; groups.extend(saved)
        # Right branch — recursively (may itself contain another `|`).
        right_pi = alt + 1
        return _match_here(pat, right_pi, s, si, flags, groups)
    if pi >= len(pat):
        return True, si
    if pat[pi] == "$" and pi + 1 == len(pat):
        return (si == len(s)), si
    if pat[pi] == "^" and pi == 0:
        if si == 0:
            return _match_here(pat, pi + 1, s, si, flags, groups)
        return False, -1
    after = _advance(pat, pi)
    nxt = pat[after] if after < len(pat) else ""
    # `??`, `*?`, `+?` lazy quantifiers
    lazy = False
    if nxt in ("*", "+", "?"):
        if after + 1 < len(pat) and pat[after + 1] == "?":
            lazy = True
    if nxt == "*":
        return _match_star(pat, pi, after + 1 + (1 if lazy else 0), s, si, flags, groups, lazy=lazy)
    if nxt == "+":
        # Save groups in case the mandatory match fails.
        saved = list(groups)
        ok, ei = _match_one(pat, pi, s, si, flags, groups)
        if not ok:
            del groups[:]; groups.extend(saved)
            return False, -1
        return _match_star(pat, pi, after + 1 + (1 if lazy else 0), s, ei, flags, groups, lazy=lazy)
    if nxt == "?":
        saved = list(groups)
        ok, ei = _match_one(pat, pi, s, si, flags, groups)
        next_pi = after + 1 + (1 if lazy else 0)
        if not lazy:
            if ok:
                ok2, ei2 = _match_here(pat, next_pi, s, ei, flags, groups)
                if ok2: return True, ei2
            del groups[:]; groups.extend(saved)
            return _match_here(pat, next_pi, s, si, flags, groups)
        else:
            # Lazy: try without first.
            saved2 = list(groups)
            del groups[:]; groups.extend(saved2)
            ok2, ei2 = _match_here(pat, next_pi, s, si, flags, groups)
            if ok2: return True, ei2
            del groups[:]; groups.extend(saved)
            if ok:
                return _match_here(pat, next_pi, s, ei, flags, groups)
            return False, -1
    # Atom + remainder.
    saved = list(groups)
    ok, ei = _match_one(pat, pi, s, si, flags, groups)
    if not ok:
        del groups[:]; groups.extend(saved)
        return False, -1
    return _match_here(pat, after, s, ei, flags, groups)


class Match:
    def __init__(self, s, start, end, groups=None):
        self._s = s
        self._start = start
        self._end = end
        self._groups = groups or []
        self._is_bytes = False
    def _b(self, v):
        if v is None: return None
        if self._is_bytes and isinstance(v, str):
            return v.encode("latin-1")
        return v
    def group(self, *args):
        if not args:
            return self._b(self._s[self._start:self._end])
        if len(args) == 1:
            n = args[0]
            if n == 0: return self._b(self._s[self._start:self._end])
            return self._b(self._groups[n - 1])
        return tuple(self.group(a) for a in args)
    def groups(self, default=None):
        return tuple((self._b(g) if g is not None else default) for g in self._groups)
    def start(self, n=0):
        return self._start
    def end(self, n=0):
        return self._end
    def span(self, n=0):
        return (self._start, self._end)
    def __bool__(self):
        return True
    def __getitem__(self, n):
        # m[0] / m[1] — CPython supports indexing on Match.
        return self.group(n)
    def groupdict(self, default=None):
        # Named-group dict; pystro's regex engine doesn't track names,
        # return an empty mapping so callers expecting a dict don't break.
        return {}
    def __repr__(self):
        return "<re.Match>"


def _strip_verbose(pat):
    """Remove unescaped whitespace and '# ...' comments from a VERBOSE
    pattern.  Mirror CPython behavior — char classes [...] and escapes
    are preserved verbatim."""
    out = []
    i = 0
    L = len(pat)
    while i < L:
        c = pat[i]
        if c == "\\" and i + 1 < L:
            out.append(pat[i:i+2]); i += 2; continue
        if c == "[":
            end = pat.find("]", i + 1)
            if end < 0: end = L - 1
            out.append(pat[i:end+1]); i = end + 1; continue
        if c in " \t\n\r":
            i += 1; continue
        if c == "#":
            # Skip to end-of-line.
            nl = pat.find("\n", i)
            i = nl + 1 if nl >= 0 else L
            continue
        out.append(c); i += 1
    return "".join(out)


def _count_groups(pat):
    """Count capturing groups in a pattern (skip (?:...), [...], \\(...)."""
    n = 0
    i = 0
    L = len(pat)
    while i < L:
        c = pat[i]
        if c == "\\" and i + 1 < L:
            i += 2
            continue
        if c == "[":
            j = pat.find("]", i + 1)
            i = (j + 1) if j >= 0 else L
            continue
        if c == "(":
            # Non-capturing variants: (?:, (?P=, (?P<, (?=, (?!, (?<=, (?<!, (?#, (?P>
            if i + 1 < L and pat[i + 1] == "?":
                if i + 2 < L and pat[i + 2] == "P" and i + 3 < L and pat[i + 3] == "<":
                    n += 1  # named group (?P<name>...)
            else:
                n += 1
            i += 1
            continue
        i += 1
    return n


def _build_group_map(pat):
    """Return dict mapping `(` position in pat to absolute group index
    (1-based for capturing groups, missing for non-capturing).

    Note: the engine usually receives a *sub-pattern* (e.g. body of a
    parent group), so positions are relative to that subpattern.  We
    rebuild the map for each parent and pass it through `_match_here`
    so children can look up their absolute index.
    """
    out = {}
    idx = 0
    i = 0
    L = len(pat)
    while i < L:
        c = pat[i]
        if c == "\\" and i + 1 < L:
            i += 2
            continue
        if c == "[":
            j = pat.find("]", i + 1)
            i = (j + 1) if j >= 0 else L
            continue
        if c == "(":
            if i + 1 < L and pat[i + 1] == "?":
                # (?P<...) is capturing; others are not
                if i + 2 < L and pat[i + 2] == "P" and i + 3 < L and pat[i + 3] == "<":
                    idx += 1
                    out[i] = idx
            else:
                idx += 1
                out[i] = idx
            i += 1
            continue
        i += 1
    return out


class Pattern:
    def __init__(self, pat, flags=0):
        # PEP-related: VERBOSE / re.X flag strips whitespace and comments
        # before regex engine sees the pattern.  Real CPython does this
        # at compile-time too.
        self._is_bytes = isinstance(pat, (bytes, bytearray))
        if self._is_bytes:
            pat = bytes(pat).decode("latin-1")
        if flags & VERBOSE:
            pat = _strip_verbose(pat)
        self.pattern = pat
        self.flags = flags
        self._n_groups = _count_groups(pat)

    @property
    def groups(self):
        return self._n_groups

    def _coerce(self, s):
        if self._is_bytes and isinstance(s, (bytes, bytearray)):
            return bytes(s).decode("latin-1")
        return s

    def _wrap(self, m, s):
        if m is None: return None
        if self._is_bytes:
            m._is_bytes = True
        # Pad groups list to declared count (alternatives may have left
        # later groups unset).
        while len(m._groups) < self._n_groups:
            m._groups.append(None)
        return m

    def _try_at(self, s, pos):
        pat = self.pattern
        anchored = pat.startswith("^")
        body = pat[1:] if anchored else pat
        groups = []
        ok, ei = _match_here(body, 0, s, pos, self.flags, groups)
        if ok:
            return Match(s, pos, ei, groups)
        return None

    def match(self, s, pos=0, endpos=None):
        s = self._coerce(s)
        return self._wrap(self._try_at(s, pos), s)

    def fullmatch(self, s, pos=0, endpos=None):
        s = self._coerce(s)
        m = self._try_at(s, pos)
        if m and m._end == len(s): return self._wrap(m, s)
        return None

    def search(self, s, pos=0, endpos=None):
        s = self._coerce(s)
        if self.pattern.startswith("^"):
            return self._wrap(self._try_at(s, pos), s)
        for i in range(pos, len(s) + 1):
            m = self._try_at(s, i)
            if m: return self._wrap(m, s)
        return None

    def findall(self, s, pos=0, endpos=None):
        out = []
        i = pos
        while i <= len(s):
            m = self.search(s, i)
            if not m: break
            if len(m._groups) == 1:
                out.append(m._groups[0])
            elif len(m._groups) > 1:
                out.append(tuple(m._groups))
            else:
                out.append(m.group())
            i = m._end if m._end > i else i + 1
        return out

    def finditer(self, s, pos=0, endpos=None):
        i = pos
        while i <= len(s):
            m = self.search(s, i)
            if not m: break
            yield m
            i = m._end if m._end > i else i + 1

    def sub(self, repl, s, count=0):
        out = []
        i = 0
        n = 0
        while i <= len(s):
            if count and n >= count:
                out.append(s[i:]); break
            m = self.search(s, i)
            if not m:
                out.append(s[i:]); break
            out.append(s[i:m._start])
            if callable(repl):
                out.append(repl(m))
            else:
                out.append(_apply_backrefs(repl, m))
            i = m._end
            if m._end == m._start: i += 1
            n += 1
        return "".join(out)

    def subn(self, repl, s, count=0):
        out = []
        i = 0; n = 0
        while i <= len(s):
            if count and n >= count:
                out.append(s[i:]); break
            m = self.search(s, i)
            if not m:
                out.append(s[i:]); break
            out.append(s[i:m._start])
            if callable(repl): out.append(repl(m))
            else: out.append(_apply_backrefs(repl, m))
            i = m._end
            if m._end == m._start: i += 1
            n += 1
        return ("".join(out), n)

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


def _apply_backrefs(repl, m):
    if "\\" not in repl: return repl
    out = []
    i = 0
    while i < len(repl):
        if repl[i] == "\\" and i + 1 < len(repl):
            n = repl[i + 1]
            if n.isdigit():
                idx = int(n)
                if idx == 0:
                    out.append(m.group(0))
                else:
                    if idx - 1 < len(m._groups):
                        out.append(m._groups[idx - 1] or "")
                i += 2
                continue
            if n == "\\":
                out.append("\\")
                i += 2
                continue
            # Other escapes: emit literal escape char
            out.append(n)
            i += 2
            continue
        out.append(repl[i])
        i += 1
    return "".join(out)


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

def subn(pattern, repl, s, count=0, flags=0):
    return compile(pattern, flags).subn(repl, s, count)

def split(pattern, s, maxsplit=0, flags=0):
    return compile(pattern, flags).split(s, maxsplit)


def escape(s):
    out = []
    for ch in s:
        if ch in r".^$*+?()[]{}|\\":
            out.append("\\")
        out.append(ch)
    return "".join(out)
