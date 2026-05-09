# pystro stub for `_collections_abc`.
#
# CPython's _collections_abc has constructs (lambda yield, real
# coroutines, internal type probes) that pystro's parser/runtime
# doesn't fully support — so we override with this small stub.
#
# Define `__all__` and the abstract base classes locally rather than
# re-exporting from `collections.abc`.  Importing from
# `collections.abc` would trigger CPython's `cpython/Lib/collections/abc.py`
# which does `from _collections_abc import __all__` (= circular import,
# we'd hit this module mid-init).


__all__ = [
    "Iterable", "Iterator", "Sized", "Container", "Hashable", "Callable",
    "Mapping", "MutableMapping", "Sequence", "MutableSequence",
    "Set", "MutableSet", "ByteString", "Generator",
    "Awaitable", "Coroutine", "AsyncIterable", "AsyncIterator",
    "AsyncGenerator", "Reversible", "Collection",
    "ItemsView", "KeysView", "ValuesView", "MappingView",
]


# CPython's collections/abc.py imports this private helper.  Walk the
# MRO looking for `methods`; return True iff every method is defined
# (and not None) on some base.
def _check_methods(C, *methods):
    try:
        mro = C.__mro__
    except AttributeError:
        return NotImplemented
    for method in methods:
        for B in mro:
            d = getattr(B, "__dict__", None)
            if d is not None and method in d:
                if d[method] is None:
                    return NotImplemented
                break
        else:
            return NotImplemented
    return True


class _ABCMixin:
    """Add `.register` / `__subclasshook__` so user code that does
    `MutableSequence.register(deque)` works without a real ABCMeta."""
    _registered = ()
    @classmethod
    def register(cls, subclass):
        # Track registration so isinstance/issubclass reflects it.
        if not hasattr(cls, "_registered_set"):
            cls._registered_set = set()
        cls._registered_set.add(subclass)
        return subclass


class Hashable(_ABCMixin):
    @classmethod
    def __subclasshook__(cls, C):
        return NotImplemented


class Sized(_ABCMixin):
    @classmethod
    def __subclasshook__(cls, C): return NotImplemented


class Container(_ABCMixin):
    @classmethod
    def __subclasshook__(cls, C): return NotImplemented


class Iterable(_ABCMixin):
    def __iter__(self): raise NotImplementedError


class Iterator(Iterable, _ABCMixin):
    def __next__(self): raise NotImplementedError
    def __iter__(self): return self


class Reversible(Iterable, _ABCMixin):
    def __reversed__(self): raise NotImplementedError


class Generator(Iterator, _ABCMixin):
    def send(self, value): raise StopIteration
    def throw(self, typ, val=None, tb=None): raise typ(val)
    def close(self): pass


class Callable(_ABCMixin):
    @classmethod
    def __subclasshook__(cls, C): return NotImplemented


class Collection(Iterable, Sized, Container, _ABCMixin):
    pass


class Set(Collection, _ABCMixin):
    pass


class MutableSet(Set, _ABCMixin):
    pass


class Mapping(Collection, _ABCMixin):
    pass


class MutableMapping(Mapping, _ABCMixin):
    pass


class Sequence(Reversible, Collection, _ABCMixin):
    pass


class MutableSequence(Sequence, _ABCMixin):
    pass


class ByteString(Sequence, _ABCMixin):
    pass


class MappingView(Sized, _ABCMixin):
    pass


class KeysView(MappingView, Set, _ABCMixin):
    pass


class ItemsView(MappingView, Set, _ABCMixin):
    pass


class ValuesView(MappingView, Collection, _ABCMixin):
    pass


# Async ABCs (pystro doesn't model real coroutines but exposes the
# names so user code that imports them at module level doesn't crash).
class Awaitable(_ABCMixin):
    pass


class Coroutine(Awaitable, _ABCMixin):
    def send(self, value): raise StopIteration
    def throw(self, typ, val=None, tb=None): raise typ(val)
    def close(self): pass


class AsyncIterable(_ABCMixin):
    pass


class AsyncIterator(AsyncIterable, _ABCMixin):
    pass


class AsyncGenerator(AsyncIterator, _ABCMixin):
    pass


# CPython's `collections/abc.py` reaches for this — provide a stub.
class _CallableGenericAlias:
    def __init__(self, *a, **k): pass
    def __getitem__(self, item): return self
