# pystro stdlib `concurrent.futures` (synchronous stub).
# Pystro has no real threads or processes, so submit/map run inline.

class CancelledError(Exception):
    pass


class TimeoutError(Exception):
    pass


class Future:
    def __init__(self):
        self._result = None
        self._exception = None
        self._done = False
        self._cancelled = False
        self._callbacks = []

    def set_result(self, value):
        self._result = value
        self._done = True
        self._call_callbacks()

    def set_exception(self, exc):
        self._exception = exc
        self._done = True
        self._call_callbacks()

    def _call_callbacks(self):
        for cb in self._callbacks:
            try:
                cb(self)
            except Exception:
                pass
        self._callbacks = []

    def add_done_callback(self, fn):
        if self._done:
            try:
                fn(self)
            except Exception:
                pass
        else:
            self._callbacks.append(fn)

    def result(self, timeout=None):
        if not self._done:
            raise TimeoutError("Future not yet done")
        if self._exception is not None:
            raise self._exception
        return self._result

    def exception(self, timeout=None):
        if not self._done:
            raise TimeoutError("Future not yet done")
        return self._exception

    def done(self):
        return self._done

    def cancelled(self):
        return self._cancelled

    def cancel(self):
        if self._done:
            return False
        self._cancelled = True
        self._done = True
        return True

    def running(self):
        return False


class _ExecutorBase:
    def submit(self, fn, /, *args, **kwargs):
        f = Future()
        try:
            f.set_result(fn(*args, **kwargs))
        except Exception as e:
            f.set_exception(e)
        return f

    def map(self, fn, *iterables, timeout=None, chunksize=1):
        for args in zip(*iterables):
            yield fn(*args)

    def shutdown(self, wait=True, *, cancel_futures=False):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.shutdown(wait=True)
        return False


class ThreadPoolExecutor(_ExecutorBase):
    def __init__(self, max_workers=None, thread_name_prefix="",
                 initializer=None, initargs=()):
        self.max_workers = max_workers


class ProcessPoolExecutor(_ExecutorBase):
    def __init__(self, max_workers=None, mp_context=None,
                 initializer=None, initargs=()):
        self.max_workers = max_workers


def as_completed(futures, timeout=None):
    """Yield futures in the order they complete.  Synchronous stub: all
    futures are already done by the time submit returned, so just yield
    in input order."""
    for f in list(futures):
        yield f


def wait(futures, timeout=None, return_when="ALL_COMPLETED"):
    """Wait for futures to complete.  Synchronous stub: everything is
    already done."""
    futures = list(futures)
    return (set(futures), set())


# Constants for `return_when`.
FIRST_COMPLETED = "FIRST_COMPLETED"
FIRST_EXCEPTION = "FIRST_EXCEPTION"
ALL_COMPLETED = "ALL_COMPLETED"


__all__ = ["Future", "ThreadPoolExecutor", "ProcessPoolExecutor",
           "as_completed", "wait", "CancelledError", "TimeoutError",
           "FIRST_COMPLETED", "FIRST_EXCEPTION", "ALL_COMPLETED"]
