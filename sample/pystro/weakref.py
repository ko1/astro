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


__all__ = ["ref", "proxy", "WeakValueDictionary", "WeakKeyDictionary"]
