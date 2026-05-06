# pystro stub for `_string` (C extension supporting string.Formatter).

def formatter_field_name_split(s):
    """Split format-field name into (first_part, iterator_of_rest)."""
    parts = s.split(".")
    return parts[0], iter([(True, p) for p in parts[1:]])


def formatter_parser(s):
    """Iterate over (literal, field_name, spec, conversion) tuples."""
    out = []
    i = 0
    n = len(s)
    while i < n:
        # Find next `{`.
        j = i
        while j < n and s[j] != "{" and s[j] != "}":
            j += 1
        literal = s[i:j]
        if j >= n:
            out.append((literal, None, None, None))
            break
        if s[j] == "}":
            # Closing without opening — treat as literal `}`.
            literal += "}"
            i = j + 1
            continue
        # `{`: scan to matching `}`.
        k = j + 1
        depth = 1
        while k < n and depth > 0:
            if s[k] == "{": depth += 1
            elif s[k] == "}": depth -= 1
            if depth == 0: break
            k += 1
        field = s[j+1:k]
        # Split off conversion (`!r`/`!s`/`!a`).
        conv = None
        if "!" in field:
            field, conv = field.split("!", 1)
            conv = conv[0] if conv else None
        # Split off spec (after `:`).
        spec = ""
        if ":" in field:
            field, spec = field.split(":", 1)
        out.append((literal, field, spec, conv))
        i = k + 1
    return iter(out)


__all__ = ["formatter_field_name_split", "formatter_parser"]
