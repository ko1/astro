# pystro stdlib `contextlib` (minimal).

class _GeneratorContextManager:
    def __init__(self, gen_func, args, kwargs):
        self._gen_func = gen_func
        self._args = args
        self._kwargs = kwargs
        self._gen = None

    def __enter__(self):
        self._gen = self._gen_func(*self._args, **self._kwargs)
        try:
            return next(self._gen)
        except StopIteration:
            raise RuntimeError("generator didn't yield")

    def __exit__(self, exc_type, exc_value, tb):
        if exc_type is None:
            try:
                next(self._gen)
            except StopIteration:
                return False
            raise RuntimeError("generator didn't stop")
        else:
            try:
                self._gen.throw(exc_type)
            except StopIteration:
                return True
            except BaseException:
                pass
            return False


def contextmanager(fn):
    def helper(*args, **kwargs):
        return _GeneratorContextManager(fn, args, kwargs)
    return helper


class suppress:
    def __init__(self, *exc_types):
        self._types = exc_types
    def __enter__(self):
        return self
    def __exit__(self, exc_type, exc_value, tb):
        if exc_type is None:
            return False
        for t in self._types:
            if issubclass(exc_type, t):
                return True
        return False


class closing:
    def __init__(self, thing):
        self.thing = thing
    def __enter__(self):
        return self.thing
    def __exit__(self, exc_type, exc_value, tb):
        try:
            self.thing.close()
        except Exception:
            pass
        return False


class nullcontext:
    def __init__(self, enter_result=None):
        self.enter_result = enter_result
    def __enter__(self):
        return self.enter_result
    def __exit__(self, *args):
        return False


class ExitStack:
    def __init__(self):
        self._stack = []
    def __enter__(self):
        return self
    def __exit__(self, exc_type, exc_value, tb):
        suppressed = False
        while self._stack:
            cb = self._stack.pop()
            try:
                if cb(exc_type, exc_value, tb):
                    suppressed = True
                    exc_type = None
                    exc_value = None
            except BaseException:
                pass
        return suppressed
    def push(self, cm):
        def _pop(et, ev, tb):
            return cm.__exit__(et, ev, tb)
        self._stack.append(_pop)
        return cm
    def enter_context(self, cm):
        result = cm.__enter__()
        def _pop(et, ev, tb):
            return cm.__exit__(et, ev, tb)
        self._stack.append(_pop)
        return result
    def callback(self, fn, *args, **kwargs):
        def _cb(et, ev, tb):
            fn(*args, **kwargs)
            return False
        self._stack.append(_cb)
        return fn
    def close(self):
        self.__exit__(None, None, None)
