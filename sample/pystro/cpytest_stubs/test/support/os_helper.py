"""Stub for test.support.os_helper."""
import os
import unittest


TESTFN = "@test"
TESTFN_ASCII = TESTFN
TESTFN_UNICODE = TESTFN
TESTFN_NONASCII = TESTFN


def unlink(path):
    try:
        os.remove(path)
    except OSError:
        pass


def rmtree(path):
    try:
        os.remove(path)
    except OSError:
        pass


def can_symlink():
    return False


def skip_unless_symlink(fn):
    return unittest.skip("no symlink")(fn)


def can_chmod():
    return False


def temp_cwd(*a, **kw):
    class _NoOp:
        def __enter__(self): return self
        def __exit__(self, *exc): return False
    return _NoOp()


def temp_dir(*a, **kw):
    class _NoOp:
        def __enter__(self): return "/tmp"
        def __exit__(self, *exc): return False
    return _NoOp()


def make_bad_fd():
    return -1


def fd_count():
    return 0


def change_cwd(path):
    class _CwdCtx:
        def __enter__(self):
            self.old = os.getcwd()
            return path
        def __exit__(self, *exc):
            return False
    return _CwdCtx()


class FakePath:
    def __init__(self, path): self.path = path
    def __fspath__(self): return self.path


__all__ = ["TESTFN", "unlink", "rmtree", "can_symlink", "skip_unless_symlink"]
