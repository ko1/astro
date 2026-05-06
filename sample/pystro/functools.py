# pystro stdlib `functools` (minimal).

class partial:
    def __init__(self, fn, *args, **kwargs):
        self.fn = fn
        self.args = args
        self.keywords = kwargs
    def __call__(self, *args, **kwargs):
        all_args = list(self.args) + list(args)
        merged_kw = dict(self.keywords)
        merged_kw.update(kwargs)
        return self.fn(*all_args, **merged_kw)


def reduce(fn, iterable, initial=None):
    it = iter(iterable)
    if initial is None:
        try:
            acc = next(it)
        except StopIteration:
            raise TypeError("reduce() of empty sequence with no initial value")
    else:
        acc = initial
    for x in it:
        acc = fn(acc, x)
    return acc


def wraps(wrapped):
    # Simplified: return a no-op decorator.  Real Python copies __name__ etc.
    def deco(fn):
        return fn
    return deco


def cache(fn):
    # Unbounded memoization keyed by args tuple.
    memo = {}
    def wrapper(*args):
        if args in memo:
            return memo[args]
        v = fn(*args)
        memo[args] = v
        return v
    return wrapper


def lru_cache(maxsize=128):
    # Accept @lru_cache or @lru_cache(maxsize=N).  When called as
    # @lru_cache (no parens), `maxsize` is the function being decorated.
    if callable(maxsize):
        # @lru_cache used directly
        return cache(maxsize)
    def deco(fn):
        memo = {}
        order = []
        def wrapper(*args):
            if args in memo:
                return memo[args]
            v = fn(*args)
            memo[args] = v
            order.append(args)
            if maxsize is not None and len(order) > maxsize:
                old = order.pop(0)
                del memo[old]
            return v
        return wrapper
    return deco


def cmp_to_key(cmp):
    class K:
        def __init__(self, obj):
            self.obj = obj
        def __lt__(self, other):
            return cmp(self.obj, other.obj) < 0
        def __eq__(self, other):
            return cmp(self.obj, other.obj) == 0
        def __le__(self, other):
            return cmp(self.obj, other.obj) <= 0
    return K


def total_ordering(cls):
    # Fill in missing rich-comparison methods given __eq__ and one of
    # __lt__, __le__, __gt__, __ge__.
    has_lt = hasattr(cls, "__lt__")
    has_le = hasattr(cls, "__le__")
    has_gt = hasattr(cls, "__gt__")
    has_ge = hasattr(cls, "__ge__")
    if has_lt:
        if not has_le:
            cls.__le__ = lambda s, o: s == o or s < o
        if not has_gt:
            cls.__gt__ = lambda s, o: not (s < o) and s != o
        if not has_ge:
            cls.__ge__ = lambda s, o: not (s < o)
    elif has_le:
        if not has_lt:
            cls.__lt__ = lambda s, o: s != o and s <= o
        if not has_gt:
            cls.__gt__ = lambda s, o: not (s <= o)
        if not has_ge:
            cls.__ge__ = lambda s, o: s == o or not (s <= o)
    elif has_gt:
        if not has_lt:
            cls.__lt__ = lambda s, o: not (s > o) and s != o
        if not has_le:
            cls.__le__ = lambda s, o: not (s > o)
        if not has_ge:
            cls.__ge__ = lambda s, o: s == o or s > o
    elif has_ge:
        if not has_lt:
            cls.__lt__ = lambda s, o: not (s >= o)
        if not has_le:
            cls.__le__ = lambda s, o: s == o or not (s >= o)
        if not has_gt:
            cls.__gt__ = lambda s, o: s != o and s >= o
    return cls


def singledispatch(fn):
    # Minimal: dispatch on type of first arg.
    registry = {}
    def dispatch(t):
        # Walk MRO of t.
        try:
            mro = t.__mro__
        except AttributeError:
            mro = [t]
        for c in mro:
            if c in registry: return registry[c]
        return fn

    def register(cls=None):
        # @proc.register or @proc.register(type)
        if callable(cls) and not isinstance(cls, type):
            # Used as @proc.register without arg — fn passed in as cls
            # (no — singledispatch.register requires a type arg in 3.7+;
            # tolerate both forms anyway)
            f = cls
            try: t = list(getattr(f, "__annotations__", {}).values())[0]
            except (IndexError, AttributeError): t = object
            registry[t] = f
            return f
        # Was called with a type: return a sub-decorator.
        def inner(f):
            registry[cls] = f
            return f
        return inner

    def wrapper(*args, **kw):
        if not args: return fn(*args, **kw)
        return dispatch(type(args[0]))(*args, **kw)
    wrapper.register = register
    wrapper.dispatch = dispatch
    wrapper.registry = registry
    return wrapper


__all__ = ["partial", "reduce", "wraps", "cache", "lru_cache",
           "cmp_to_key", "total_ordering", "singledispatch"]
