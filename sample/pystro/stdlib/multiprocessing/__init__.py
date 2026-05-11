# pystro stdlib `multiprocessing` — minimal stub.
#
# Pystro is single-process; we only expose enough surface for tests to
# import without crashing.  Anything that actually spawns / forks raises
# SkipTest at runtime.
import unittest as _unittest


def cpu_count():
    return 1


def current_process():
    class _P:
        name = "MainProcess"
        daemon = False
        pid = 0
        def is_alive(self): return True
    return _P()


def active_children():
    return []


class Process:
    def __init__(self, target=None, args=(), kwargs=None, name=None, daemon=None):
        self._target = target
        self._args = args
        self._kwargs = kwargs or {}
        self.name = name or "Process"
        self.daemon = bool(daemon)
        self.pid = None
        self.exitcode = None
    def start(self):
        if self._target is not None:
            self._target(*self._args, **self._kwargs)
    def join(self, timeout=None): pass
    def terminate(self): pass
    def is_alive(self): return False


def Queue(*a, **kw):
    import queue
    return queue.Queue()


def Pipe(duplex=True):
    return (None, None)


def Lock(*a, **kw):
    import threading
    return threading.Lock()


def RLock(*a, **kw):
    import threading
    return threading.RLock()


def Manager():
    raise _unittest.SkipTest("multiprocessing.Manager not supported in pystro")


def get_context(method=None):
    from . import context
    return context._default_context


def set_start_method(method, force=False):
    pass


def get_start_method(allow_none=False):
    return "fork"
