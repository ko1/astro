"""pystro stub for `_weakref` (the C accelerator for `weakref`).

pystro has no weak-ref tracking — Boehm's conservative scanner keeps
anything reachable.  We expose the API CPython's `weakref.py` reaches
for; refs never spontaneously become None, but user code that does
`ref() is None` only after explicit `del` still works."""


class ref:
    """CPython exposes `ref` as a class (so `class KeyedRef(ref):` works).
    pystro doesn't track weak references — Boehm's conservative scanner
    keeps anything reachable — but we expose the API surface used by
    `weakref.py` (`__call__` to fetch referent, `__hash__`, `__eq__`)."""
    __slots__ = ("_target", "_callback")
    def __init__(self, target, callback=None):
        self._target = target
        self._callback = callback
    def __new__(cls, target, callback=None, *a, **kw):
        # CPython's ref.__new__ accepts subclass extra args (KeyedRef has
        # a 3rd `key` positional).  Discard them; subclass __init__ /
        # __new__ takes care of attribute setup.
        obj = object.__new__(cls)
        return obj
    def __call__(self):
        return self._target
    def __hash__(self):
        return hash(id(self._target))
    def __eq__(self, other):
        return isinstance(other, ref) and self._target is other._target


# Back-compat alias — CPython's `weakref.ReferenceType` IS `_weakref.ref`.
ReferenceType = ref


class ProxyType:
    __slots__ = ("_target",)
    def __init__(self, target):
        self._target = target
    def __getattr__(self, name):
        return getattr(self._target, name)
    def __call__(self, *a, **k):
        return self._target(*a, **k)


class CallableProxyType(ProxyType):
    pass


def proxy(target, callback=None):
    if callable(target):
        return CallableProxyType(target)
    return ProxyType(target)


def getweakrefcount(obj):
    return 0


def getweakrefs(obj):
    return []


def _remove_dead_weakref(d, key):
    # CPython hook for WeakValueDictionary cleanup.  Pystro keeps strong
    # refs throughout; just discard the entry if present.
    try: del d[key]
    except (KeyError, TypeError): pass


__all__ = ["ref", "ReferenceType", "ProxyType", "CallableProxyType",
           "getweakrefcount", "getweakrefs", "proxy"]
