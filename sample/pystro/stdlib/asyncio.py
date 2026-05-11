# pystro stdlib `asyncio` (synchronous stub).
#
# Pystro does not yet have a real coroutine / event-loop implementation.
# To allow code that uses `async def` / `await` to run, this module
# provides a synchronous stub:
#   - `async def f(): ...` is parsed but treated as a regular def
#     (pystro's parser already accepts `async`)
#   - `await x` evaluates `x` (no actual suspension)
#   - `asyncio.run(coro)` calls `coro` if it's callable, else returns it
#   - `asyncio.gather(*aws)` returns a list of values
#   - `asyncio.sleep(secs)` calls `time.sleep`

import time as _time


def _drain(g):
    """If g is a generator (pystro async def → gen when body has yield),
    iterate to completion and return the StopIteration.value."""
    if hasattr(g, "send") and hasattr(g, "close"):
        try:
            while True:
                next(g)
        except StopIteration as e:
            return getattr(e, "value", None)
    return g


def run(coro):
    if callable(coro):
        coro = coro()
    return _drain(coro)


def gather(*coros):
    out = []
    for c in coros:
        if callable(c):
            c = c()
        out.append(_drain(c))
    return out


def sleep(seconds):
    return _time.sleep(seconds)


def create_task(coro):
    if callable(coro):
        return coro()
    return coro


def ensure_future(coro):
    return coro


def wait_for(coro, timeout=None):
    if callable(coro):
        return coro()
    return coro


# Lock / Event / Semaphore / Queue stubs.
class Lock:
    def __init__(self):
        self._locked = False
    def __enter__(self):
        self._locked = True
        return self
    def __exit__(self, *a):
        self._locked = False
    def acquire(self):
        self._locked = True
    def release(self):
        self._locked = False
    def locked(self):
        return self._locked


class Event:
    def __init__(self): self._set = False
    def set(self): self._set = True
    def clear(self): self._set = False
    def is_set(self): return self._set
    def wait(self): pass


class Queue:
    def __init__(self, maxsize=0):
        self._items = []
    def put(self, item): self._items.append(item)
    def put_nowait(self, item): self._items.append(item)
    def get(self):
        if self._items: return self._items.pop(0)
        return None
    def get_nowait(self):
        if self._items: return self._items.pop(0)
        return None
    def empty(self): return len(self._items) == 0
    def qsize(self): return len(self._items)


def iscoroutinefunction(fn):
    """Pystro treats `async def` as a regular generator function; if
    `fn.is_async` is reachable, use it; otherwise heuristic-check
    whether the function was declared with `async def`."""
    flag = getattr(fn, "is_async", None)
    if flag is not None:
        return bool(flag)
    return getattr(fn, "_is_coroutine", False)


def iscoroutine(obj):
    return iscoroutinefunction(obj) or (
        hasattr(obj, "send") and hasattr(obj, "throw") and hasattr(obj, "close"))


def isfuture(obj):
    return False


def get_event_loop():
    return _DummyLoop()


class _DummyLoop:
    def run_until_complete(self, coro):
        return run(coro)
    def close(self): pass


__all__ = [
    "run", "gather", "sleep", "create_task", "ensure_future", "wait_for",
    "Lock", "Event", "Queue",
    "get_event_loop",
]


# Various stdlib / test modules access `asyncio.staggered` even though
# they never invoke it (e.g. probing the module surface).  Pystro is
# sync-only so the racer can't actually do anything useful; expose a
# stub object whose attribute access succeeds.
class _StaggeredStub:
    def staggered_race(self, coro_fns, delay, *, loop=None):
        for fn in coro_fns:
            try:
                v = fn() if callable(fn) else fn
                return v, 0, [None] * len(coro_fns)
            except Exception:
                continue
        return None, None, []
staggered = _StaggeredStub()
