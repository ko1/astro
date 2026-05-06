"""pystro stub for `unicodedata` (CPython C extension).  Most Unicode
property queries return defaults — pystro doesn't ship the Unicode
database."""

unidata_version = "15.1.0"


def name(chr_, default=None):
    if default is None:
        raise ValueError("no name")
    return default


def lookup(name):
    raise KeyError(name)


def category(chr_):
    if not chr_: return "Cn"
    cp = ord(chr_)
    if 0x30 <= cp <= 0x39: return "Nd"
    if 0x41 <= cp <= 0x5A: return "Lu"
    if 0x61 <= cp <= 0x7A: return "Ll"
    if cp in (0x20,): return "Zs"
    if 0x21 <= cp <= 0x2F: return "Po"
    return "Cn"


def bidirectional(chr_):
    return ""


def east_asian_width(chr_):
    return "N"


def combining(chr_):
    return 0


def decimal(chr_, default=None):
    if not chr_: return default
    cp = ord(chr_)
    if 0x30 <= cp <= 0x39: return cp - 0x30
    if default is None: raise ValueError("not a decimal")
    return default


def digit(chr_, default=None):
    return decimal(chr_, default)


def numeric(chr_, default=None):
    if not chr_: return default
    cp = ord(chr_)
    if 0x30 <= cp <= 0x39: return float(cp - 0x30)
    if default is None: raise ValueError("not numeric")
    return default


def mirrored(chr_):
    return 0


def decomposition(chr_):
    return ""


def normalize(form, unistr):
    return unistr


def is_normalized(form, unistr):
    return True


ucd_3_2_0 = None


__all__ = ["name", "lookup", "category", "bidirectional", "east_asian_width",
           "combining", "decimal", "digit", "numeric", "mirrored",
           "decomposition", "normalize", "is_normalized", "unidata_version"]
