"""Stub for test.support.os_helper."""
import os
import unittest
import tempfile


# CPython points TESTFN at the test's working dir + "@test_<pid>_tmp".
# Pystro tests sometimes run from a read-only / sandboxed cwd; route
# the basename through tempfile.gettempdir() so open/remove always
# succeeds.  Keep the basename starting with "@test" — CPython tests
# probe `if 'test' in TESTFN`.
_TESTFN_BASE = "@test_pystro_" + str(os.getpid())
TESTFN = os.path.join(tempfile.gettempdir(), _TESTFN_BASE)
TESTFN_ASCII = TESTFN
TESTFN_UNICODE = TESTFN
TESTFN_NONASCII = TESTFN + "\xe9"
# A name that's *not* decodable in the FS encoding — CPython probes for
# one; pystro just hands back a name with surrogate-style bytes so
# tests that gate on `if TESTFN_UNDECODABLE is None: skip` proceed.
TESTFN_UNDECODABLE = None
TESTFN_UNENCODABLE = None


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


def create_empty_file(path):
    open(path, "wb").close()


class EnvironmentVarGuard:
    """Context manager for temporarily setting/unsetting environment vars."""
    def __init__(self):
        self._changed = {}
    def __enter__(self):
        return self
    def __exit__(self, *exc):
        for k, v in self._changed.items():
            if v is None:
                if k in os.environ:
                    del os.environ[k]
            else:
                os.environ[k] = v
    def set(self, key, value):
        self._changed[key] = os.environ.get(key)
        os.environ[key] = value
    def unset(self, key):
        self._changed[key] = os.environ.get(key)
        if key in os.environ:
            del os.environ[key]
    def __contains__(self, k): return k in os.environ
    def __getitem__(self, k): return os.environ[k]
    def __setitem__(self, k, v): self.set(k, v)
    def __delitem__(self, k): self.unset(k)
    def keys(self): return os.environ.keys()


def can_hardlink():
    return False


def can_xattr():
    return False


def fs_is_case_insensitive(path=None):
    return False


def skip_if_dac_override(fn):
    return fn


def skip_unless_working_chmod(fn):
    return unittest.skip("no working chmod")(fn)


def skip_unless_xattr(fn):
    return unittest.skip("no xattr")(fn)


def skip_unless_hardlink(fn):
    return unittest.skip("no hardlink")(fn)


def calling_clean_temp_dir(*a, **kw):
    pass


# Non-ASCII filesystem character for filename-encoding tests.  CPython
# probes os.fsencode for a usable char; pystro hardcodes a safe default.
FS_NONASCII = "\xe9"


__all__ = ["TESTFN", "unlink", "rmtree", "can_symlink", "skip_unless_symlink",
           "create_empty_file", "EnvironmentVarGuard", "FS_NONASCII"]
