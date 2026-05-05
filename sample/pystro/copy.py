# pystro stdlib `copy` (minimal).

def copy(obj):
    # Shallow copy.
    if isinstance(obj, list):
        return obj[:]
    if isinstance(obj, tuple):
        return tuple(obj)
    if isinstance(obj, dict):
        return obj.copy()
    if isinstance(obj, set):
        return set(obj)
    # User-defined class: clone instance's __dict__-like attrs.
    cls = obj.__class__ if hasattr(obj, "__class__") else None
    # Fallback: use obj as-is (copy.copy of immutables = identity).
    return obj


def deepcopy(obj, memo=None):
    if memo is None:
        memo = {}
    oid = id(obj)
    if oid in memo:
        return memo[oid]
    if isinstance(obj, list):
        r = []
        memo[oid] = r
        for x in obj:
            r.append(deepcopy(x, memo))
        return r
    if isinstance(obj, tuple):
        # tuples are immutable; deepcopy of immutable contents.
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
    # Immutables (int, str, float, None, bool) — return as-is.
    return obj

__all__ = ["copy", "deepcopy"]
