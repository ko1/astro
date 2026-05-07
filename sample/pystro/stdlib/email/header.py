"""pystro stub for `email.header`."""


class Header:
    def __init__(self, s="", charset=None, *args, **kw):
        self._s = str(s)
        self._charset = charset
    def __str__(self): return self._s
    def encode(self, splitchars=";, \t", maxlinelen=None, linesep="\n"):
        return self._s
    def append(self, s, charset=None, errors="strict"):
        self._s += str(s)


def decode_header(header):
    if header is None: return [(b"", None)]
    return [(str(header).encode("ascii", "replace"), None)]


def make_header(decoded_seq, maxlinelen=None, header_name=None, continuation_ws=" "):
    h = Header()
    for s, charset in decoded_seq:
        if isinstance(s, bytes):
            s = s.decode("ascii", "replace")
        h.append(s, charset)
    return h


__all__ = ["Header", "decode_header", "make_header"]
