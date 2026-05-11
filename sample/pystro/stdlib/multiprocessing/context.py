# pystro stdlib `multiprocessing.context` — minimal stub.

class BaseContext:
    Process = None  # set below
    def get_context(self, method=None):
        return self
    def Queue(self, maxsize=0):
        import queue
        return queue.Queue(maxsize)
    def Lock(self):
        import threading
        return threading.Lock()
    def RLock(self):
        import threading
        return threading.RLock()
    def Pipe(self, duplex=True):
        return (None, None)
    def cpu_count(self):
        return 1
    def Event(self):
        import threading
        return threading.Event()


class DefaultContext(BaseContext):
    pass


import multiprocessing as _mp
BaseContext.Process = _mp.Process


_default_context = DefaultContext()


# multiprocessing.context.reduction — CPython exposes a `reduction`
# submodule for pickle-based fd / lock transfer; pystro is single-process
# so wire it as a no-op.
import types as _types
reduction = _types.ModuleType("multiprocessing.context.reduction")


def _register(*args, **kw): pass


reduction.register = _register
reduction.ForkingPickler = type("ForkingPickler", (), {
    "register": staticmethod(_register),
    "dumps": staticmethod(lambda obj, protocol=None: b""),
    "loadbuf": staticmethod(lambda buf: None),
})


import sys as _sys
_sys.modules['multiprocessing.context.reduction'] = reduction
_sys.modules['multiprocessing.reduction'] = reduction
