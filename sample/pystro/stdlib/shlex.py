# pystro stdlib `shlex` (minimal): shell-like lexical analysis.

def split(s, comments=False, posix=True):
    """Split shell-style: respect single/double quotes."""
    if s is None: return []
    out = []
    cur = []
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c.isspace():
            if cur:
                out.append("".join(cur))
                cur = []
            i += 1
            continue
        if comments and c == "#":
            break
        if c == "\\" and posix and i + 1 < n:
            cur.append(s[i+1])
            i += 2
            continue
        if c == '"':
            i += 1
            while i < n and s[i] != '"':
                if posix and s[i] == "\\" and i + 1 < n and s[i+1] in '"\\':
                    cur.append(s[i+1])
                    i += 2
                else:
                    cur.append(s[i])
                    i += 1
            i += 1  # skip closing quote
            continue
        if c == "'":
            i += 1
            while i < n and s[i] != "'":
                cur.append(s[i])
                i += 1
            i += 1
            continue
        cur.append(c)
        i += 1
    if cur:
        out.append("".join(cur))
    return out


def quote(s):
    """Return shell-escaped version of s."""
    if not s: return "''"
    if all(c.isalnum() or c in "@%+=:,./-_" for c in s):
        return s
    return "'" + s.replace("'", "'\\''") + "'"


def join(split_command):
    return " ".join(quote(arg) for arg in split_command)


__all__ = ["split", "quote", "join"]
