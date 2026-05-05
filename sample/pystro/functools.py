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


__all__ = ["partial", "reduce", "wraps", "cache", "lru_cache", "cmp_to_key"]
