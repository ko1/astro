"""pystro stub for `atexit`."""

_handlers = []


def register(func, *args, **kwargs):
    _handlers.append((func, args, kwargs))
    return func


def unregister(func):
    global _handlers
    _handlers = [(f, a, k) for f, a, k in _handlers if f is not func]


def _run_exitfuncs():
    while _handlers:
        f, a, k = _handlers.pop()
        try: f(*a, **k)
        except Exception: pass


def _clear():
    _handlers.clear()


__all__ = ["register", "unregister"]
