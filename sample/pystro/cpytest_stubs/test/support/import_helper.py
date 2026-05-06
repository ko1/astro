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


__all__ = ["import_module", "import_fresh_module", "unload",
           "CleanImport", "DirsOnSysPath"]
