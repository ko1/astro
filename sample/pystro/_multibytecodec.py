"""pystro stub for `_multibytecodec` (codec base C accelerator)."""


class MultibyteCodec:
    pass


class MultibyteIncrementalEncoder:
    def __init__(self, errors="strict"):
        self.errors = errors
    def encode(self, s, final=False): return s.encode("utf-8")
    def reset(self): pass


class MultibyteIncrementalDecoder:
    def __init__(self, errors="strict"):
        self.errors = errors
    def decode(self, b, final=False):
        return b.decode("utf-8") if isinstance(b, (bytes, bytearray)) else b
    def reset(self): pass


class MultibyteStreamReader:
    def __init__(self, stream, errors="strict"):
        self.stream = stream
        self.errors = errors


class MultibyteStreamWriter:
    def __init__(self, stream, errors="strict"):
        self.stream = stream
        self.errors = errors


def __create_codec(*args, **kwargs):
    return None


__all__ = ["MultibyteCodec", "MultibyteIncrementalEncoder",
           "MultibyteIncrementalDecoder", "MultibyteStreamReader",
           "MultibyteStreamWriter"]
