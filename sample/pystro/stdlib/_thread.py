"""pystro stub for `_thread` (CPython threading C extension)."""
import time as _time


error = RuntimeError
TIMEOUT_MAX = 9999999.0
LOCK_FAILURE = 0


def get_ident():
    return 1


def get_native_id():
    return 1


def allocate_lock():
    return _Lock()


class _Lock:
    def __init__(self):
        self._locked = False
    def acquire(self, blocking=True, timeout=-1):
        if self._locked:
            if blocking and timeout > 0:
                _time.sleep(timeout)
            if not blocking: return False
            return False
        self._locked = True
        return True
    def release(self):
        if not self._locked:
            raise RuntimeError("release unlocked lock")
        self._locked = False
    def locked(self):
        return self._locked
    def __enter__(self):
        self.acquire()
        return self
    def __exit__(self, *exc):
        self.release()
        return False
    def _at_fork_reinit(self):
        # Single-process pystro: no fork, no work to do.
        self._locked = False


LockType = _Lock


# Reentrant lock — pystro is single-threaded, so every acquire from the
# "same thread" succeeds immediately.  CPython's threading module
# uses RLock for nested acquires (see logging._acquireLock).
class RLock:
    def __init__(self):
        self._depth = 0
    def acquire(self, blocking=True, timeout=-1):
        self._depth += 1
        return True
    def release(self):
        if self._depth <= 0:
            raise RuntimeError("cannot release un-acquired lock")
        self._depth -= 1
    def locked(self):
        return self._depth > 0
    def __enter__(self):
        self.acquire()
        return self
    def __exit__(self, *exc):
        self.release()
        return False
    def _is_owned(self):
        return self._depth > 0
    def _at_fork_reinit(self):
        self._depth = 0


def allocate_RLock():
    return RLock()


def start_new_thread(fn, args, kwargs=None):
    fn(*args, **(kwargs or {}))
    return 1


def stack_size(size=0):
    return 0


def interrupt_main():
    raise KeyboardInterrupt


_count = lambda: 1


class _local:
    pass


def get_main_thread_id():
    return 1


def daemon_threads_allowed(): return True


def _is_main_interpreter():
    return True


def _set_sentinel():
    return _Lock()


def _make_thread_handle(ident):
    return _ThreadHandle()


class _ThreadHandle:
    def join(self, timeout=None): pass
    def is_done(self): return True
    def _set_done(self): pass
    @property
    def ident(self): return 1


def _shutdown():
    pass


def _excepthook(args):
    pass


def _get_main_thread_ident():
    return 1


def _ExceptHookArgs(args):
    return args


__all__ = ["error", "TIMEOUT_MAX", "get_ident", "get_native_id",
           "allocate_lock", "LockType", "start_new_thread",
           "stack_size", "interrupt_main", "_local"]
