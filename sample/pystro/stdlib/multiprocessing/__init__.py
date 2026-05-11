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


def get_all_start_methods():
    return ["fork"]


def set_executable(path):
    pass


# multiprocessing.pool submodule placeholder.
import types as _types_pool
_pool_mod = _types_pool.ModuleType("multiprocessing.pool")
class _PoolStub:
    def __init__(self, *args, **kwargs): pass
    def apply(self, func, args=(), kwds={}): return func(*args, **kwds)
    def apply_async(self, *args, **kwargs): return self
    def map(self, func, iterable, chunksize=None): return [func(x) for x in iterable]
    def map_async(self, *args, **kwargs): return self
    def close(self): pass
    def join(self): pass
    def terminate(self): pass
    def __enter__(self): return self
    def __exit__(self, *e): return False
_pool_mod.Pool = _PoolStub
_pool_mod.ThreadPool = _PoolStub
import sys as _sys_pool
_sys_pool.modules['multiprocessing.pool'] = _pool_mod
pool = _pool_mod


# multiprocessing.shared_memory — PEP 580 / 3.8+ shared memory blocks.
import types as _types_sm
_sm_mod = _types_sm.ModuleType("multiprocessing.shared_memory")
class _SharedMemoryStub:
    def __init__(self, name=None, create=False, size=0, **kw):
        self.name = name or "shm_stub"
        self.size = size
        self.buf = bytearray(size)
    def close(self): pass
    def unlink(self): pass

class _ShareableListStub:
    def __init__(self, sequence=None, *, name=None):
        self._items = list(sequence or [])
        self.shm = _SharedMemoryStub()
    def __len__(self): return len(self._items)
    def __getitem__(self, i): return self._items[i]
    def __setitem__(self, i, v): self._items[i] = v

_sm_mod.SharedMemory = _SharedMemoryStub
_sm_mod.ShareableList = _ShareableListStub
import sys as _sys_sm
_sys_sm.modules['multiprocessing.shared_memory'] = _sm_mod
shared_memory = _sm_mod


# multiprocessing.managers - minimal stub.
import types as _types_mgr
_mgr_mod = _types_mgr.ModuleType("multiprocessing.managers")
class _BaseManagerStub:
    def __init__(self, *args, **kwargs): pass
    def start(self): pass
    def shutdown(self): pass
    def __enter__(self): return self
    def __exit__(self, *e): return False
_mgr_mod.BaseManager = _BaseManagerStub
_mgr_mod.SyncManager = _BaseManagerStub
import sys as _sys_mgr
_sys_mgr.modules['multiprocessing.managers'] = _mgr_mod
managers = _mgr_mod


# Exception types CPython exposes at top-level.
class AuthenticationError(Exception):
    pass


class BufferTooShort(Exception):
    pass


class ProcessError(Exception):
    pass


class TimeoutError(Exception):
    pass


# multiprocessing.connection submodule placeholder.
import sys as _sys
import types as _types
_conn_mod = _types.ModuleType("multiprocessing.connection")
_conn_mod.Listener = None
_conn_mod.Client = None
class _ListenerStub:
    def __init__(self, *args, **kwargs): pass
    def accept(self): raise NotImplementedError
    def close(self): pass
class _ClientStub:
    def __init__(self, *args, **kwargs): pass
_conn_mod.Listener = _ListenerStub
_conn_mod.Client = _ClientStub
_conn_mod.XmlListener = _ListenerStub
_conn_mod.XmlClient = _ClientStub
_conn_mod.AuthenticationError = AuthenticationError
_conn_mod.BufferTooShort = BufferTooShort
_conn_mod.deliver_challenge = lambda *a, **k: None
_conn_mod.answer_challenge = lambda *a, **k: None
_sys.modules['multiprocessing.connection'] = _conn_mod
connection = _conn_mod
