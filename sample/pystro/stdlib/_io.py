"""pystro stub for `_io` (the C accelerator for io)."""

from io import (
    IOBase, RawIOBase, BufferedIOBase, TextIOBase,
    FileIO, BytesIO, StringIO, BufferedReader, BufferedWriter,
    BufferedRandom, BufferedRWPair, TextIOWrapper, IncrementalNewlineDecoder,
    UnsupportedOperation, BlockingIOError,
    DEFAULT_BUFFER_SIZE,
)


def open(*args, **kwargs):
    import builtins
    return builtins.open(*args, **kwargs)


def open_code(path):
    return open(path, "rb")


__all__ = ["IOBase", "RawIOBase", "BufferedIOBase", "TextIOBase",
           "FileIO", "BytesIO", "StringIO", "BufferedReader", "BufferedWriter",
           "BufferedRandom", "BufferedRWPair", "TextIOWrapper",
           "IncrementalNewlineDecoder", "UnsupportedOperation",
           "BlockingIOError", "DEFAULT_BUFFER_SIZE", "open", "open_code"]
