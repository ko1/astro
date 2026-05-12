"""pystro stub for the frozen importlib._bootstrap_external module."""


class _NamespacePath:
    def __init__(self, name, path, getter=None):
        self._name = name
        self._path = path
    def __iter__(self): return iter(self._path)
    def __len__(self): return len(self._path)


class FileFinder:
    @staticmethod
    def path_hook(*loaders):
        def hook(path):
            return FileFinder(path)
        return hook
    def __init__(self, path): self._path = path
    def find_spec(self, name, target=None): return None


class SourceFileLoader:
    def __init__(self, name, path):
        self.name = name
        self.path = path


class ExtensionFileLoader:
    def __init__(self, name, path):
        self.name = name
        self.path = path


class SourcelessFileLoader:
    def __init__(self, name, path):
        self.name = name
        self.path = path


EXTENSION_SUFFIXES = []
SOURCE_SUFFIXES = [".py"]
BYTECODE_SUFFIXES = [".pyc"]

# Path separator constants used by importlib's bootstrap code.
import os as _os
path_sep = _os.sep
path_separators = [_os.sep]
if _os.altsep:
    path_separators.append(_os.altsep)
_path_separators = "".join(path_separators)
del _os


def _unpack_uint16(data):
    return int.from_bytes(data[:2], "little")


def _unpack_uint32(data):
    return int.from_bytes(data[:4], "little")


def _pack_uint16(value):
    return int(value).to_bytes(2, "little")


def _pack_uint32(value):
    return int(value).to_bytes(4, "little")


def _classify_pyc(*args, **kwargs):
    return None


def _validate_hash_pyc(*args, **kwargs):
    return None


def _validate_timestamp_pyc(*args, **kwargs):
    return None


MAGIC_NUMBER = b"\x55\x0d\x0d\x0a"


__all__ = ["_NamespacePath", "FileFinder", "SourceFileLoader",
           "ExtensionFileLoader", "SourcelessFileLoader",
           "EXTENSION_SUFFIXES", "SOURCE_SUFFIXES", "BYTECODE_SUFFIXES",
           "_unpack_uint16", "_unpack_uint32", "_pack_uint16", "_pack_uint32",
           "MAGIC_NUMBER"]
