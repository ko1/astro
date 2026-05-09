"""pystro stub for `_weakref` (the C accelerator for `weakref`).

pystro has no weak-ref tracking — Boehm's conservative scanner keeps
anything reachable.  We expose the API CPython's `weakref.py` reaches
for; refs never spontaneously become None, but user code that does
`ref() is None` only after explicit `del` still works."""


class ReferenceType:
    __slots__ = ("_target", "_callback")
    def __init__(self, target, callback=None):
        self._target = target
        self._callback = callback
    def __call__(self):
        return self._target
    def __hash__(self):
        return hash(id(self._target))
    def __eq__(self, other):
        return isinstance(other, ReferenceType) and self._target is other._target


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


def ref(target, callback=None):
    return ReferenceType(target, callback)


def proxy(target, callback=None):
    if callable(target):
        return CallableProxyType(target)
    return ProxyType(target)


def getweakrefcount(obj):
    return 0


def getweakrefs(obj):
    return []


__all__ = ["ref", "ReferenceType", "ProxyType", "CallableProxyType",
           "getweakrefcount", "getweakrefs", "proxy"]
