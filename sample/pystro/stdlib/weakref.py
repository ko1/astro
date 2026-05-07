# pystro stdlib `weakref` (minimal, no actual weak semantics).
#
# pystro uses Boehm GC; we don't currently expose weak refs to Python.
# This stub returns a strong reference wrapped in a callable, so basic
# patterns (`w = ref(obj); w() is obj`) work.  Live-after-collection
# tests will report incorrect results.

class _Ref:
    def __init__(self, obj, callback=None):
        self._obj = obj
        self._callback = callback
    def __call__(self):
        return self._obj


def ref(obj, callback=None):
    return _Ref(obj, callback)


def proxy(obj, callback=None):
    return obj


class WeakValueDictionary(dict):
    pass


class WeakKeyDictionary(dict):
    pass


ReferenceType = _Ref
ProxyType = type(None)
CallableProxyType = type(None)


def getweakrefcount(obj):
    return 0


def getweakrefs(obj):
    return []


class WeakSet(set):
    pass


class WeakMethod(_Ref):
    pass


class finalize:
    def __init__(self, obj, func, *args, **kwargs):
        self._func = func
        self._args = args
        self._kwargs = kwargs
        self._alive = True
    def __call__(self):
        if self._alive:
            self._alive = False
            return self._func(*self._args, **self._kwargs)
    def detach(self):
        self._alive = False
        return None
    @property
    def alive(self):
        return self._alive


__all__ = ["ref", "proxy", "WeakValueDictionary", "WeakKeyDictionary",
           "ReferenceType", "ProxyType", "CallableProxyType",
           "getweakrefcount", "getweakrefs", "WeakSet", "WeakMethod",
           "finalize"]
