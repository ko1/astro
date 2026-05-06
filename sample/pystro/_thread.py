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


LockType = _Lock


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


def daemon_threads_allowed():
    return False


def _is_main_interpreter():
    return True


__all__ = ["error", "TIMEOUT_MAX", "get_ident", "get_native_id",
           "allocate_lock", "LockType", "start_new_thread",
           "stack_size", "interrupt_main", "_local"]
