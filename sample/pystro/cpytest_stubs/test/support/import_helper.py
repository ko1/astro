"""Stub for test.support.import_helper."""
import sys
import unittest


def import_module(name, *, required_on=None, deprecated=False):
    try:
        return __import__(name)
    except ImportError as e:
        raise unittest.SkipTest("no " + name + ": " + str(e))


def import_fresh_module(name, fresh=(), blocked=(), *, deprecated=False):
    try:
        return __import__(name)
    except ImportError as e:
        raise unittest.SkipTest("no " + name)


def unload(name):
    if name in sys.modules:
        del sys.modules[name]


class CleanImport:
    def __init__(self, *names):
        self.names = names
    def __enter__(self): return self
    def __exit__(self, *exc): return False


class DirsOnSysPath:
    def __init__(self, *dirs):
        self.dirs = dirs
    def __enter__(self):
        for d in self.dirs:
            sys.path.insert(0, d)
        return self
    def __exit__(self, *exc):
        for d in self.dirs:
            try: sys.path.remove(d)
            except ValueError: pass
        return False


def make_legacy_pyc(filename):
    return filename


def forget(modname):
    if modname in sys.modules:
        del sys.modules[modname]


def ensure_lazy_imports(*names):
    pass


def modules_setup():
    return ()


def modules_cleanup(*a, **kw):
    pass


def isolated_modules():
    class _Ctx:
        def __enter__(self): return self
        def __exit__(self, *e): return False
    return _Ctx()


def frozen_modules(enabled=True):
    """No-op context manager — pystro has no frozen modules."""
    class _Ctx:
        def __enter__(self): return self
        def __exit__(self, *e): return False
    return _Ctx()


def multi_interp_extensions_check(enabled=True):
    class _Ctx:
        def __enter__(self): return self
        def __exit__(self, *e): return False
    return _Ctx()


def unlink(filename):
    """CPython's import_helper.unlink — same as os.unlink with EEXIST/
    FileNotFoundError swallowed.  Used by tests that probe pyc cache."""
    import os
    try:
        os.unlink(filename)
    except (FileNotFoundError, OSError):
        pass


def uncache(*names):
    """Context manager that removes given module names from sys.modules
    on entry and restores them on exit."""
    class _Ctx:
        def __init__(self):
            self.saved = {}
            for n in names:
                if n in sys.modules:
                    self.saved[n] = sys.modules[n]
                    del sys.modules[n]
        def __enter__(self): return self
        def __exit__(self, *exc):
            for n, m in self.saved.items():
                sys.modules[n] = m
            return False
    return _Ctx()


__all__ = ["import_module", "import_fresh_module", "unload",
           "CleanImport", "DirsOnSysPath", "ensure_lazy_imports",
           "modules_setup", "modules_cleanup", "isolated_modules",
           "frozen_modules", "multi_interp_extensions_check"]
