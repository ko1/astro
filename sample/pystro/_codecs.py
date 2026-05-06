"""pystro stub for `_codecs` (CPython codec C extension).  Provides
minimal ASCII / UTF-8 / latin-1 codecs and registry stubs."""


# Codec lookup registry.
_codec_search_path = []
_codec_cache = {}


def register(search_function):
    _codec_search_path.append(search_function)


def unregister(search_function):
    if search_function in _codec_search_path:
        _codec_search_path.remove(search_function)


_ALIASES = {
    "ascii": "ascii", "us-ascii": "ascii", "646": "ascii",
    "utf-8": "utf-8", "utf8": "utf-8", "u8": "utf-8",
    "utf_8": "utf-8", "u-8": "utf-8",
    "latin-1": "latin-1", "latin1": "latin-1", "iso-8859-1": "latin-1",
    "iso8859-1": "latin-1", "iso_8859_1": "latin-1", "8859": "latin-1",
    "cp1252": "latin-1",      # rough fallback
    "utf-16": "utf-16", "utf16": "utf-16",
    "utf-32": "utf-32", "utf32": "utf-32",
}


def lookup(encoding):
    enc = encoding.lower().replace("_", "-")
    canonical = _ALIASES.get(enc)
    if canonical is None:
        # Try registered search functions.
        for fn in _codec_search_path:
            r = fn(enc)
            if r is not None: return r
        raise LookupError("unknown encoding: " + encoding)
    return _CodecInfo(canonical)


class _CodecInfo:
    def __init__(self, name):
        self.name = name
    @property
    def encode(self):
        if self.name == "ascii": return ascii_encode
        if self.name == "utf-8": return utf_8_encode
        if self.name == "latin-1": return latin_1_encode
        return utf_8_encode
    @property
    def decode(self):
        if self.name == "ascii": return ascii_decode
        if self.name == "utf-8": return utf_8_decode
        if self.name == "latin-1": return latin_1_decode
        return utf_8_decode


# Per-codec encode/decode pairs.
def ascii_encode(s, errors="strict"):
    if not isinstance(s, str): s = str(s)
    out = []
    for c in s:
        if ord(c) < 128: out.append(c)
        else:
            if errors == "ignore": continue
            if errors == "replace": out.append("?"); continue
            raise UnicodeEncodeError("ascii", s, 0, len(s), "ordinal not in range(128)")
    return ("".join(out).encode() if False else b"".join(c.encode() if False else bytes([ord(c)]) for c in out)), len(s)


def ascii_decode(b, errors="strict"):
    if isinstance(b, bytes): pass
    else: b = bytes(b)
    out = []
    for x in b:
        if x < 128: out.append(chr(x))
        else:
            if errors == "ignore": continue
            if errors == "replace": out.append("�"); continue
            raise UnicodeDecodeError("ascii", b, 0, len(b), "ordinal not in range(128)")
    return "".join(out), len(b)


def utf_8_encode(s, errors="strict"):
    if not isinstance(s, str): s = str(s)
    out = bytearray()
    for c in s:
        cp = ord(c)
        if cp < 0x80: out.append(cp)
        elif cp < 0x800:
            out.append(0xC0 | (cp >> 6)); out.append(0x80 | (cp & 0x3F))
        elif cp < 0x10000:
            out.append(0xE0 | (cp >> 12)); out.append(0x80 | ((cp >> 6) & 0x3F)); out.append(0x80 | (cp & 0x3F))
        else:
            out.append(0xF0 | (cp >> 18)); out.append(0x80 | ((cp >> 12) & 0x3F))
            out.append(0x80 | ((cp >> 6) & 0x3F)); out.append(0x80 | (cp & 0x3F))
    return bytes(out), len(s)


def utf_8_decode(b, errors="strict", final=True):
    if not isinstance(b, (bytes, bytearray)):
        try: b = bytes(b)
        except: b = bytes()
    s = b.decode() if hasattr(b, "decode") and False else None
    # Python-side decode walking UTF-8.
    out = []
    i = 0
    n = len(b)
    while i < n:
        x = b[i]
        if x < 0x80:
            out.append(chr(x)); i += 1
        elif (x & 0xE0) == 0xC0 and i + 1 < n:
            cp = ((x & 0x1F) << 6) | (b[i+1] & 0x3F); out.append(chr(cp)); i += 2
        elif (x & 0xF0) == 0xE0 and i + 2 < n:
            cp = ((x & 0x0F) << 12) | ((b[i+1] & 0x3F) << 6) | (b[i+2] & 0x3F)
            out.append(chr(cp)); i += 3
        elif (x & 0xF8) == 0xF0 and i + 3 < n:
            cp = ((x & 0x07) << 18) | ((b[i+1] & 0x3F) << 12) | ((b[i+2] & 0x3F) << 6) | (b[i+3] & 0x3F)
            out.append(chr(cp)); i += 4
        else:
            if errors == "ignore": i += 1; continue
            if errors == "replace": out.append("�"); i += 1; continue
            raise UnicodeDecodeError("utf-8", b, i, i+1, "invalid byte")
    return "".join(out), n


def latin_1_encode(s, errors="strict"):
    if not isinstance(s, str): s = str(s)
    out = bytearray()
    for c in s:
        cp = ord(c)
        if cp < 256: out.append(cp)
        else:
            if errors == "ignore": continue
            if errors == "replace": out.append(ord("?")); continue
            raise UnicodeEncodeError("latin-1", s, 0, len(s), "ordinal not in range(256)")
    return bytes(out), len(s)


def latin_1_decode(b, errors="strict"):
    if not isinstance(b, (bytes, bytearray)): b = bytes(b)
    return "".join(chr(x) for x in b), len(b)


utf_16_encode = utf_8_encode
utf_16_decode = utf_8_decode
utf_32_encode = utf_8_encode
utf_32_decode = utf_8_decode

charmap_encode = latin_1_encode
charmap_decode = latin_1_decode

raw_unicode_escape_encode = latin_1_encode
raw_unicode_escape_decode = latin_1_decode
unicode_escape_encode = latin_1_encode
unicode_escape_decode = latin_1_decode

mbcs_encode = latin_1_encode
mbcs_decode = latin_1_decode

readbuffer_encode = lambda s, errors="strict": (bytes(s, "utf-8") if isinstance(s, str) else bytes(s), len(s))


def escape_encode(s, errors="strict"):
    return repr(s)[1:-1].encode("ascii"), len(s)


def escape_decode(s, errors="strict"):
    if isinstance(s, str): s = s.encode("ascii")
    return s.decode("ascii"), len(s)


def code_page_encode(*a, **kw): return latin_1_encode(*a[1:], **kw) if len(a)>0 else (b"", 0)
def code_page_decode(*a, **kw): return latin_1_decode(*a[1:], **kw) if len(a)>0 else ("", 0)


def encode(obj, encoding="utf-8", errors="strict"):
    return lookup(encoding).encode(obj, errors)[0]


def decode(obj, encoding="utf-8", errors="strict"):
    return lookup(encoding).decode(obj, errors)[0]


# These aren't really part of _codecs but ASCII handlers reference them
# for error handlers.
def register_error(name, error_handler): pass
def lookup_error(name): return None


__all__ = ["register", "unregister", "lookup", "encode", "decode",
           "ascii_encode", "ascii_decode", "utf_8_encode", "utf_8_decode",
           "latin_1_encode", "latin_1_decode",
           "charmap_encode", "charmap_decode",
           "register_error", "lookup_error"]
