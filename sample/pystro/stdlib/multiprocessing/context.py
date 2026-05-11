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
