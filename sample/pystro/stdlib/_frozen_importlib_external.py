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


__all__ = ["_NamespacePath", "FileFinder", "SourceFileLoader",
           "ExtensionFileLoader", "SourcelessFileLoader",
           "EXTENSION_SUFFIXES", "SOURCE_SUFFIXES", "BYTECODE_SUFFIXES"]
