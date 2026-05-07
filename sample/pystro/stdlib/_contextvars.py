"""pystro stub for `_contextvars` (single-threaded context implementation)."""

_NO_DEFAULT = object()


class Token:
    MISSING = object()
    def __init__(self, var, old):
        self._var = var
        self._old_value = old
    @property
    def var(self):
        return self._var
    @property
    def old_value(self):
        return self._old_value


class ContextVar:
    def __init__(self, name, *, default=_NO_DEFAULT):
        self._name = name
        self._default = default
        self._value = default
    @property
    def name(self):
        return self._name
    def get(self, *args):
        if self._value is _NO_DEFAULT:
            if args:
                return args[0]
            if self._default is _NO_DEFAULT:
                raise LookupError(self._name)
            return self._default
        return self._value
    def set(self, value):
        old = self._value
        self._value = value
        return Token(self, old)
    def reset(self, token):
        self._value = token._old_value


class Context:
    def __init__(self):
        self._vars = {}
    def run(self, func, *args, **kwargs):
        return func(*args, **kwargs)
    def copy(self):
        c = Context()
        c._vars = dict(self._vars)
        return c
    def __getitem__(self, var):
        return var.get()
    def __contains__(self, var):
        return var._value is not _NO_DEFAULT
    def __iter__(self):
        return iter(self._vars)
    def __len__(self):
        return len(self._vars)
    def keys(self): return self._vars.keys()
    def values(self): return self._vars.values()
    def items(self): return self._vars.items()
    def get(self, var, default=None):
        return var.get(default)


def copy_context():
    return Context()


__all__ = ["ContextVar", "Context", "Token", "copy_context"]
