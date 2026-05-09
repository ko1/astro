# pystro stdlib `importlib` — surface area CPython tests use most.

import sys


def import_module(name, package=None):
    return __import__(name)


def reload(module):
    name = getattr(module, "__name__", None)
    if name and name in sys.modules:
        del sys.modules[name]
    return __import__(name) if name else module


def invalidate_caches():
    pass


class _ModuleSpec:
    def __init__(self, name, origin=None, loader=None):
        self.name = name
        self.origin = origin
        self.loader = loader
        self.submodule_search_locations = None
        self.has_location = origin is not None
        self.cached = None
        self.parent = name.rpartition(".")[0] if "." in name else ""


def find_spec(name, package=None, target=None):
    if name in sys.modules:
        m = sys.modules[name]
        return _ModuleSpec(name, getattr(m, "__file__", None))
    return None


# CPython's `importlib._bootstrap` is C-resident internals.  Tests reach
# for it; provide a fake module-like so attribute access doesn't crash.
class _BootstrapModule:
    __name__ = "importlib._bootstrap"
    def acquire_lock(self, *a, **k): pass
    def release_lock(self, *a, **k): pass
    def _resolve_name(self, name, package, level):
        if level == 0: return name
        bits = package.split(".") if package else []
        if level > len(bits):
            raise ImportError("attempted relative import beyond top-level package")
        base = ".".join(bits[:len(bits) - level + 1])
        return base + ("." + name if name else "")


_bootstrap = _BootstrapModule()


class _BootstrapExternal:
    __name__ = "importlib._bootstrap_external"
    def _path_importer_cache(self, *a, **k):
        return None


_bootstrap_external = _BootstrapExternal()


class _Util:
    __name__ = "importlib.util"
    def find_spec(self, name, package=None):
        return find_spec(name, package)
    def module_from_spec(self, spec):
        import types
        m = types.ModuleType(spec.name)
        m.__spec__ = spec
        if spec.origin: m.__file__ = spec.origin
        return m
    def spec_from_file_location(self, name, location, loader=None,
                                submodule_search_locations=None):
        return _ModuleSpec(name, location, loader)
    LazyLoader = None
    def cache_from_source(self, path, debug_override=None, optimization=None):
        return path
    def source_from_cache(self, path):
        return path


util = _Util()


class _Machinery:
    __name__ = "importlib.machinery"
    SOURCE_SUFFIXES = [".py"]
    DEBUG_BYTECODE_SUFFIXES = [".pyc"]
    OPTIMIZED_BYTECODE_SUFFIXES = [".pyc"]
    BYTECODE_SUFFIXES = [".pyc"]
    EXTENSION_SUFFIXES = [".so"]
    ModuleSpec = _ModuleSpec
    class SourceFileLoader:
        def __init__(self, name, path):
            self.name = name; self.path = path
    class ExtensionFileLoader:
        def __init__(self, name, path):
            self.name = name; self.path = path
    class FileFinder:
        def __init__(self, path, *loaders):
            self.path = path
        @classmethod
        def path_hook(cls, *loaders):
            return lambda path: cls(path, *loaders)


machinery = _Machinery()


class _Abc:
    __name__ = "importlib.abc"
    class Loader: pass
    class Finder: pass
    class MetaPathFinder: pass
    class PathEntryFinder: pass
    class ResourceLoader: pass
    class ExecutionLoader: pass
    class FileLoader: pass
    class SourceLoader: pass
    class InspectLoader: pass


abc = _Abc()


class _Resources:
    __name__ = "importlib.resources"
    def files(self, package): return None
    def open_text(self, package, resource, encoding="utf-8"):
        raise FileNotFoundError(resource)
    def read_text(self, package, resource, encoding="utf-8"):
        raise FileNotFoundError(resource)
    def open_binary(self, package, resource):
        raise FileNotFoundError(resource)
    def read_binary(self, package, resource):
        raise FileNotFoundError(resource)


resources = _Resources()


__all__ = ["import_module", "reload", "invalidate_caches", "find_spec",
           "_bootstrap", "_bootstrap_external", "util", "machinery",
           "abc", "resources"]
