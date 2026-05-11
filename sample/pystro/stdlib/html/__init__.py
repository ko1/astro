# pystro stdlib `html` — minimal stub.
#
# CPython's html.unescape uses a regex with brace quantifier {1,32},
# which pystro's bundled re engine doesn't support.  Implement the
# same semantics with a hand-written scanner over the html5 dict.

from html.entities import html5 as _html5

# Code points that CPython remaps for invalid charrefs.
_invalid_charrefs = {
    0x00: "�", 0x0d: "\r", 0x80: "€", 0x81: "\x81",
    0x82: "‚", 0x83: "ƒ", 0x84: "„", 0x85: "…",
    0x86: "†", 0x87: "‡", 0x88: "ˆ", 0x89: "‰",
    0x8a: "Š", 0x8b: "‹", 0x8c: "Œ", 0x8d: "\x8d",
    0x8e: "Ž", 0x8f: "\x8f", 0x90: "\x90", 0x91: "‘",
    0x92: "’", 0x93: "“", 0x94: "”", 0x95: "•",
    0x96: "–", 0x97: "—", 0x98: "˜", 0x99: "™",
    0x9a: "š", 0x9b: "›", 0x9c: "œ", 0x9d: "\x9d",
    0x9e: "ž", 0x9f: "Ÿ",
}

_invalid_codepoints = {
    # 0x0001 to 0x0008
    0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8,
    # 0x000E to 0x001F
    0xe, 0xf, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
    0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    # 0x007F to 0x009F
    0x7f, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a,
    0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
    # 0xFDD0 to 0xFDEF
    0xfdd0, 0xfdd1, 0xfdd2, 0xfdd3, 0xfdd4, 0xfdd5, 0xfdd6, 0xfdd7, 0xfdd8,
    0xfdd9, 0xfdda, 0xfddb, 0xfddc, 0xfddd, 0xfdde, 0xfddf, 0xfde0, 0xfde1,
    0xfde2, 0xfde3, 0xfde4, 0xfde5, 0xfde6, 0xfde7, 0xfde8, 0xfde9, 0xfdea,
    0xfdeb, 0xfdec, 0xfded, 0xfdee, 0xfdef,
    # others
    0xb, 0xfffe, 0xffff, 0x1fffe, 0x1ffff, 0x2fffe, 0x2ffff,
    0x3fffe, 0x3ffff, 0x4fffe, 0x4ffff, 0x5fffe, 0x5ffff,
    0x6fffe, 0x6ffff, 0x7fffe, 0x7ffff, 0x8fffe, 0x8ffff,
    0x9fffe, 0x9ffff, 0xafffe, 0xaffff, 0xbfffe, 0xbffff,
    0xcfffe, 0xcffff, 0xdfffe, 0xdffff, 0xefffe, 0xeffff,
    0xffffe, 0xfffff, 0x10fffe, 0x10ffff,
}


def _decode_numeric(s):
    """Decode &#NNN; or &#xHHH; (no leading &#); returns char or None
    if s is malformed.  Trailing ';' optional."""
    if not s or s[0] != '#':
        return None
    s = s[1:].rstrip(';')
    try:
        if s and s[0] in 'xX':
            num = int(s[1:], 16)
        else:
            num = int(s)
    except Exception:
        return None
    if num in _invalid_charrefs:
        return _invalid_charrefs[num]
    if 0xD800 <= num <= 0xDFFF or num > 0x10FFFF:
        return '�'
    if num in _invalid_codepoints:
        return ''
    return chr(num)


def escape(s, quote=True):
    s = s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
    if quote:
        s = s.replace('"', '&quot;').replace("'", '&#x27;')
    return s


def unescape(s):
    """Mirror CPython's html.unescape semantics without regex."""
    if '&' not in s:
        return s
    out = []
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c != '&':
            out.append(c)
            i += 1
            continue
        # Numeric: &#NNN; / &#xHHH;
        if i + 1 < n and s[i+1] == '#':
            j = i + 2
            if j < n and s[j] in 'xX':
                k = j + 1
                while k < n and (s[k].isdigit() or s[k] in 'abcdefABCDEF'):
                    k += 1
            else:
                k = j
                while k < n and s[k].isdigit():
                    k += 1
            if k == j or (k == j + 1 and s[j] in 'xX'):
                # Incomplete numeric — emit literal &# or &#x.
                out.append(s[i:k])
                i = k
                continue
            has_semi = (k < n and s[k] == ';')
            ref = s[i+1:k] + (';' if has_semi else '')
            decoded = _decode_numeric(ref)
            if decoded is None:
                out.append(s[i:k + (1 if has_semi else 0)])
            else:
                out.append(decoded)
            i = k + (1 if has_semi else 0)
            continue
        # Named: &name; — look up the longest match in _html5 (entries
        # have an explicit trailing ';' or not).  CPython considers up
        # to 32 chars past the &.
        end = min(n, i + 1 + 32)
        # Scan forward; track best (longest) match.
        best = None
        best_len = 0
        for j in range(i + 2, end + 1):
            cand = s[i+1:j]
            if cand in _html5:
                best = _html5[cand]
                best_len = j - (i + 1)
        if best is not None:
            out.append(best)
            i = i + 1 + best_len
        else:
            out.append('&')
            i += 1
    return "".join(out)


__all__ = ["escape", "unescape"]
