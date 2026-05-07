# pystro stdlib `glob` (minimal): file pattern matching.
import os as _os
import fnmatch as _fnmatch


def glob(pathname, *, recursive=False):
    """List files matching pathname pattern."""
    return list(iglob(pathname, recursive=recursive))


def iglob(pathname, *, recursive=False):
    """Iterator version."""
    if not pathname:
        return
    if "*" not in pathname and "?" not in pathname and "[" not in pathname:
        if _os.path.exists(pathname):
            yield pathname
        return
    # Split into directory + name pattern.
    dirname, basename = _os.path.split(pathname)
    if not dirname:
        # Pattern in current dir.
        try:
            entries = _os.listdir(".")
        except Exception:
            return
        for n in entries:
            if _fnmatch.fnmatch(n, basename):
                yield n
        return
    # If the dirname itself contains wildcards, recurse on dirname.
    if "*" in dirname or "?" in dirname or "[" in dirname:
        for d in iglob(dirname, recursive=recursive):
            sub = _os.path.join(d, basename)
            for m in iglob(sub, recursive=recursive):
                yield m
        return
    try:
        entries = _os.listdir(dirname)
    except Exception:
        return
    for n in entries:
        if _fnmatch.fnmatch(n, basename):
            yield _os.path.join(dirname, n)


def escape(pathname):
    """Escape glob metacharacters in pathname."""
    out = []
    for c in pathname:
        if c in "*?[":
            out.append("[" + c + "]")
        else:
            out.append(c)
    return "".join(out)


__all__ = ["glob", "iglob", "escape"]
