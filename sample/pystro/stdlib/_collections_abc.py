# pystro stub for `_collections_abc`.  CPython's _collections_abc has
# constructs (lambda yield, async def) and CPython-internal type
# probes that pystro's parser/runtime doesn't support.  Re-export from
# our `collections.abc`.

from collections.abc import (
    Iterable, Iterator, Sized, Container, Hashable, Callable,
    Mapping, MutableMapping, Sequence, MutableSequence,
    Set, MutableSet, ByteString, Generator,
)

# Additional names CPython uses internally.
class Awaitable: pass
class Coroutine: pass
class AsyncIterable: pass
class AsyncIterator: pass
class AsyncGenerator: pass
class Reversible(Iterable): pass
class Collection(Iterable, Sized, Container): pass
class ItemsView(Set): pass
class KeysView(Set): pass
class ValuesView(Collection): pass
class MappingView: pass


__all__ = [
    "Iterable", "Iterator", "Sized", "Container", "Hashable", "Callable",
    "Mapping", "MutableMapping", "Sequence", "MutableSequence",
    "Set", "MutableSet", "ByteString", "Generator",
    "Awaitable", "Coroutine", "AsyncIterable", "AsyncIterator",
    "AsyncGenerator", "Reversible", "Collection",
    "ItemsView", "KeysView", "ValuesView", "MappingView",
]
