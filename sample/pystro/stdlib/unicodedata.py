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


# Mini-namespace for the Unicode 3.2.0 database — used by `stringprep`
# (RFC 3454).  Pystro doesn't actually load a separate UCD, so the
# functions delegate to the module-level helpers above, and the
# version string is the literal '3.2.0' that stringprep asserts on.
class _UCD320:
    unidata_version = "3.2.0"
    name = staticmethod(name)
    lookup = staticmethod(lookup)
    category = staticmethod(category)
    bidirectional = staticmethod(bidirectional)
    east_asian_width = staticmethod(east_asian_width)
    combining = staticmethod(combining)
    decimal = staticmethod(decimal)
    digit = staticmethod(digit)
    numeric = staticmethod(numeric)
    mirrored = staticmethod(mirrored)
    decomposition = staticmethod(decomposition)
    normalize = staticmethod(normalize)
    is_normalized = staticmethod(is_normalized)

ucd_3_2_0 = _UCD320()


__all__ = ["name", "lookup", "category", "bidirectional", "east_asian_width",
           "combining", "decimal", "digit", "numeric", "mirrored",
           "decomposition", "normalize", "is_normalized", "unidata_version"]
