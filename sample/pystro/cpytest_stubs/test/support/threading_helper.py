"""Stub for test.support.threading_helper."""
import unittest


def reap_threads(fn):
    return fn


def threading_setup(*a, **kw): return ()
def threading_cleanup(*a, **kw): pass


def join_thread(thread, timeout=None): pass


def start_threads(threads, unlock=None):
    class _Ctx:
        def __enter__(self): return self
        def __exit__(self, *e): return False
    return _Ctx()


def wait_threads_exit(timeout=None):
    class _Ctx:
        def __enter__(self): return self
        def __exit__(self, *e): return False
    return _Ctx()


def requires_working_threading(*, module=False):
    if module:
        raise unittest.SkipTest("threading not supported")
    def deco(fn): return unittest.skip("threading")(fn)
    return deco


def can_start_thread():
    return True


def catch_threading_exception():
    class _Ctx:
        def __enter__(self): return self
        def __exit__(self, *e): return False
    return _Ctx()


__all__ = ["reap_threads", "threading_setup", "threading_cleanup",
           "join_thread", "start_threads", "wait_threads_exit",
           "requires_working_threading", "can_start_thread",
           "catch_threading_exception"]
