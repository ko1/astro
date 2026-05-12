"""pystro stub for the frozen importlib._bootstrap module."""


class _ModuleLock:
    def __init__(self, name): self.name = name
    def acquire(self, *a, **kw): return True
    def release(self): pass
    def has_deadlock(self): return False


class _DummyModuleLock(_ModuleLock):
    pass


class _ModuleLockManager:
    def __init__(self, name): self.name = name
    def __enter__(self): return self
    def __exit__(self, *exc): return False


class BuiltinImporter:
    @classmethod
    def find_spec(cls, name, path=None, target=None):
        return None


class FrozenImporter:
    @classmethod
    def find_spec(cls, name, path=None, target=None):
        return None


def _resolve_name(name, package, level):
    return name


def _find_and_load(name, import_):
    return import_(name)


def _calc___package__(globals):
    return globals.get("__package__", "")


_bootstrap_module = None


__all__ = ["_ModuleLock", "_DummyModuleLock", "_ModuleLockManager",
           "BuiltinImporter", "FrozenImporter",
           "_resolve_name", "_find_and_load", "_calc___package__"]
