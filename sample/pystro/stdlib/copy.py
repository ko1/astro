# pystro stdlib `copy` (minimal).

def copy(obj):
    # Honor __copy__ hook.
    hook = getattr(obj, "__copy__", None)
    if hook is not None:
        return hook()
    # Shallow copy.
    if isinstance(obj, list):
        return obj[:]
    if isinstance(obj, tuple):
        return tuple(obj)
    if isinstance(obj, dict):
        return obj.copy()
    if isinstance(obj, set):
        return set(obj)
    if isinstance(obj, frozenset):
        return frozenset(obj)
    # User-defined class instance: clone with shallow attr copy.
    cls = type(obj)
    if hasattr(obj, "__dict__"):
        try:
            new = cls.__new__(cls)
        except TypeError:
            return obj
        try:
            new.__dict__.update(obj.__dict__)
        except (TypeError, AttributeError):
            pass
        return new
    return obj


def deepcopy(obj, memo=None):
    if memo is None:
        memo = {}
    oid = id(obj)
    if oid in memo:
        return memo[oid]
    # Honor __deepcopy__ hook.
    hook = getattr(obj, "__deepcopy__", None)
    if hook is not None:
        r = hook(memo)
        memo[oid] = r
        return r
    if isinstance(obj, list):
        r = []
        memo[oid] = r
        for x in obj:
            r.append(deepcopy(x, memo))
        return r
    if isinstance(obj, tuple):
        return tuple(deepcopy(x, memo) for x in obj)
    if isinstance(obj, dict):
        r = {}
        memo[oid] = r
        for k, v in obj.items():
            r[deepcopy(k, memo)] = deepcopy(v, memo)
        return r
    if isinstance(obj, set):
        r = set()
        memo[oid] = r
        for x in obj:
            r.add(deepcopy(x, memo))
        return r
    if isinstance(obj, frozenset):
        r = frozenset(deepcopy(x, memo) for x in obj)
        memo[oid] = r
        return r
    # User class instance: deepcopy its __dict__.
    if hasattr(obj, "__dict__"):
        cls = type(obj)
        try:
            new = cls.__new__(cls)
        except TypeError:
            return obj
        memo[oid] = new
        try:
            for k, v in obj.__dict__.items():
                new.__dict__[k] = deepcopy(v, memo)
        except (TypeError, AttributeError):
            pass
        return new
    # Immutables (int, str, float, None, bool, bytes) — return as-is.
    return obj

__all__ = ["copy", "deepcopy"]
