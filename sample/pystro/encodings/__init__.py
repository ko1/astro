# pystro stub for `encodings` package.

from encodings import aliases


def normalize_encoding(encoding):
    if isinstance(encoding, bytes):
        encoding = encoding.decode("ascii", "replace")
    return encoding.replace("_", "-").lower()


def search_function(encoding):
    """No real codec search — pystro's _codecs lookup handles core
    encodings; everything else fails."""
    try:
        import _codecs
        return _codecs.lookup(encoding)
    except LookupError:
        return None


# CPython registers this with codecs.register at startup.
import codecs as _codecs_mod
try:
    _codecs_mod.register(search_function)
except Exception:
    pass
