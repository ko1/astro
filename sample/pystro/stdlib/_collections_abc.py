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


class Hashable:
    @classmethod
    def __subclasshook__(cls, C):
        return NotImplemented


class Sized:
    @classmethod
    def __subclasshook__(cls, C): return NotImplemented


class Container:
    @classmethod
    def __subclasshook__(cls, C): return NotImplemented


class Iterable:
    def __iter__(self): raise NotImplementedError


class Iterator(Iterable):
    def __next__(self): raise NotImplementedError
    def __iter__(self): return self


class Reversible(Iterable):
    def __reversed__(self): raise NotImplementedError


class Generator(Iterator):
    def send(self, value): raise StopIteration
    def throw(self, typ, val=None, tb=None): raise typ(val)
    def close(self): pass


class Callable:
    @classmethod
    def __subclasshook__(cls, C): return NotImplemented


class Collection(Iterable, Sized, Container):
    pass


class Set(Collection):
    pass


class MutableSet(Set):
    pass


class Mapping(Collection):
    pass


class MutableMapping(Mapping):
    pass


class Sequence(Reversible, Collection):
    pass


class MutableSequence(Sequence):
    pass


class ByteString(Sequence):
    pass


class MappingView(Sized):
    pass


class KeysView(MappingView, Set):
    pass


class ItemsView(MappingView, Set):
    pass


class ValuesView(MappingView, Collection):
    pass


# Async ABCs (pystro doesn't model real coroutines but exposes the
# names so user code that imports them at module level doesn't crash).
class Awaitable:
    pass


class Coroutine(Awaitable):
    def send(self, value): raise StopIteration
    def throw(self, typ, val=None, tb=None): raise typ(val)
    def close(self): pass


class AsyncIterable:
    pass


class AsyncIterator(AsyncIterable):
    pass


class AsyncGenerator(AsyncIterator):
    pass


# CPython's `collections/abc.py` reaches for this — provide a stub.
class _CallableGenericAlias:
    def __init__(self, *a, **k): pass
    def __getitem__(self, item): return self
