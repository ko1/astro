# pystro stdlib `threading` — single-threaded stub.
#
# pystro has no threads.  The API matches CPython's enough for code
# that uses Lock as a CM or Thread as a "do later" wrapper.

class _Lock:
    def __init__(self):
        self._held = False
    def acquire(self, *a, **k):
        self._held = True
        return True
    def release(self):
        self._held = False
    def __enter__(self):
        self.acquire()
        return self
    def __exit__(self, *a):
        self.release()
        return False
    def locked(self):
        return self._held


def Lock():
    return _Lock()


def RLock():
    return _Lock()


class Event:
    def __init__(self):
        self._set = False
    def set(self): self._set = True
    def clear(self): self._set = False
    def is_set(self): return self._set
    def wait(self, timeout=None): return self._set


class Thread:
    def __init__(self, target=None, args=(), kwargs=None, name=None, daemon=None):
        self.target = target
        self.args = args
        self.kwargs = kwargs or {}
        self.name = name or "Thread"
        self.daemon = daemon
        self._started = False
        self._done = False
    def start(self):
        # Single-threaded: run synchronously.
        self._started = True
        if self.target:
            self.target(*self.args, **self.kwargs)
        self._done = True
    def join(self, timeout=None):
        return None
    def is_alive(self):
        return self._started and not self._done


def current_thread():
    t = Thread()
    t.name = "MainThread"
    return t


def get_ident():
    return 1
