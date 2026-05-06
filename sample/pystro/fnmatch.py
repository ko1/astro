# pystro stdlib `fnmatch` (minimal): shell-style wildcard matching.
import re as _re


def fnmatch(name, pattern):
    """Test name against pattern.  Case-sensitive (CPython is case-
    insensitive on Windows; pystro is Linux-only)."""
    return fnmatchcase(name, pattern)


def fnmatchcase(name, pattern):
    return bool(_compile(pattern).match(name))


def filter(names, pattern):
    rx = _compile(pattern)
    return [n for n in names if rx.match(n)]


def translate(pattern):
    """Convert shell pattern to regex."""
    out = []
    i = 0
    n = len(pattern)
    while i < n:
        c = pattern[i]
        if c == "*":
            out.append(".*")
            i += 1
        elif c == "?":
            out.append(".")
            i += 1
        elif c == "[":
            # Character class.  Find closing ].
            j = i + 1
            if j < n and pattern[j] == "!":
                j += 1
            if j < n and pattern[j] == "]":
                j += 1
            while j < n and pattern[j] != "]":
                j += 1
            if j >= n:
                out.append("\\[")
                i += 1
            else:
                stuff = pattern[i+1:j]
                if stuff[:1] == "!":
                    stuff = "^" + stuff[1:]
                stuff = stuff.replace("\\", "\\\\")
                out.append("[" + stuff + "]")
                i = j + 1
        else:
            # Escape regex metachars.
            if c in "\\.+(){}|^$":
                out.append("\\" + c)
            else:
                out.append(c)
            i += 1
    return "".join(out) + "$"


_compiled_cache = {}


def _compile(pattern):
    if pattern not in _compiled_cache:
        _compiled_cache[pattern] = _re.compile(translate(pattern))
    return _compiled_cache[pattern]


__all__ = ["fnmatch", "fnmatchcase", "filter", "translate"]
