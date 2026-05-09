"""Additional R17 compat: union types, paren-import, exec writeback,
class annotations carry values, memoryview slicing, MappingProxyType,
fnmatch, glob, shlex, IntEnum arithmetic, async-as-sync, gen exc."""
import unittest


class UnionTypeTest(unittest.TestCase):
    def test_int_str_union(self):
        u = int | str
        self.assertIsInstance(5, u)
        self.assertIsInstance("x", u)
        self.assertNotIsInstance(3.14, u)

    def test_three_way(self):
        u = int | str | float
        self.assertIsInstance(3.14, u)
        self.assertIsInstance(5, u)
        self.assertIsInstance("a", u)


class ParenImportTest(unittest.TestCase):
    def test_paren_imports(self):
        from collections import (
            OrderedDict,
            Counter,
            defaultdict,
        )
        self.assertEqual(OrderedDict.__name__, "OrderedDict")
        self.assertEqual(Counter.__name__, "Counter")
        self.assertEqual(defaultdict.__name__, "defaultdict")


class ExecWritebackTest(unittest.TestCase):
    def test_exec_modifies_ns(self):
        ns = {}
        exec("a = 5; b = a * 2", ns)
        self.assertEqual(ns.get("a"), 5)
        self.assertEqual(ns.get("b"), 10)

    def test_eval_with_globals(self):
        self.assertEqual(eval("x + 1", {"x": 10}), 11)


class AnnotationValueTest(unittest.TestCase):
    def test_class_annotations_have_types(self):
        class P:
            x: int
            y: str = "hi"
        self.assertEqual(P.__annotations__["x"], int)
        self.assertEqual(P.__annotations__["y"], str)

    def test_dataclass_annotations(self):
        from dataclasses import dataclass
        @dataclass
        class Q:
            a: int
            b: float = 0.0
        self.assertEqual(Q.__annotations__["a"], int)
        self.assertEqual(Q.__annotations__["b"], float)


class MemoryViewTest(unittest.TestCase):
    def test_memoryview_basic(self):
        mv = memoryview(b"hello")
        self.assertEqual(mv[0], ord("h"))
        self.assertEqual(len(mv), 5)
        self.assertEqual(bytes(mv[1:4]), b"ell")

    def test_memoryview_slice_chained(self):
        mv = memoryview(b"abcdef")[1:5]
        self.assertEqual(bytes(mv), b"bcde")
        self.assertEqual(bytes(mv[1:3]), b"cd")


class MappingProxyTest(unittest.TestCase):
    def test_readonly(self):
        from types import MappingProxyType
        m = MappingProxyType({"a": 1, "b": 2})
        self.assertEqual(m["a"], 1)
        self.assertEqual(len(m), 2)
        with self.assertRaises(TypeError):
            m["c"] = 3


class FnmatchGlobTest(unittest.TestCase):
    def test_fnmatch(self):
        import fnmatch
        self.assertTrue(fnmatch.fnmatch("foo.txt", "*.txt"))
        self.assertFalse(fnmatch.fnmatch("foo.py", "*.txt"))
        self.assertEqual(fnmatch.filter(["a.txt", "b.py"], "*.txt"), ["a.txt"])

    def test_glob_present(self):
        import glob
        self.assertTrue(callable(glob.glob))


class ShlexTest(unittest.TestCase):
    def test_split_quoted(self):
        import shlex
        self.assertEqual(shlex.split('a "b c" d'), ["a", "b c", "d"])

    def test_quote_join(self):
        import shlex
        self.assertEqual(shlex.quote("hello world"), "'hello world'")


class IntEnumArithTest(unittest.TestCase):
    def test_intenum_add(self):
        from enum import IntEnum
        class IF(IntEnum):
            X = 1
            Y = 2
        self.assertEqual(IF.X + 1, 2)
        self.assertEqual(int(IF.X), 1)
        self.assertTrue(IF.X < IF.Y)


class AsyncCoroutineShapeTest(unittest.TestCase):
    # CPython compat: `async def` returns a coroutine wrapper.  pystro
    # doesn't run an event loop but exposes the close/send/throw/__await__
    # methods so import-time `(async def f())().close()` works (CPython's
    # types.py / _collections_abc.py rely on this).
    def test_async_def_returns_coroutine(self):
        async def f(): return 42
        result = f()
        self.assertEqual(type(result).__name__, "coroutine")

    def test_coroutine_close_is_noop(self):
        async def f(): return 99
        result = f()
        result.close()  # no-op, no exception


class GeneratorExceptionTest(unittest.TestCase):
    def test_typeerror_in_yield_from_propagates(self):
        def g():
            yield from 5
        with self.assertRaises(TypeError):
            list(g())

    def test_user_iter_with_inner_stopiter(self):
        class It:
            def __init__(self, src): self._iter = iter(src)
            def __iter__(self): return self
            def __next__(self): return next(self._iter)
        self.assertEqual(list(It([1, 2, 3])), [1, 2, 3])


class CollectionsAbcMetaTest(unittest.TestCase):
    def test_iter_mapping_seq(self):
        import collections.abc as cabc
        self.assertIsInstance({}, cabc.Mapping)
        self.assertIsInstance([1, 2], cabc.Iterable)
        self.assertIsInstance("abc", cabc.Sequence)
        self.assertIsInstance(lambda: 1, cabc.Callable)


class UrllibParseTest(unittest.TestCase):
    def test_quote_unquote(self):
        from urllib.parse import quote, unquote
        self.assertEqual(quote("a b"), "a%20b")
        self.assertEqual(unquote("a%20b"), "a b")

    def test_urlparse(self):
        from urllib.parse import urlparse
        p = urlparse("https://example.com:8080/path?q=1")
        self.assertEqual(p.scheme, "https")
        self.assertEqual(p.hostname, "example.com")
        self.assertEqual(p.port, 8080)
        self.assertEqual(p.path, "/path")


class TypingExtraTest(unittest.TestCase):
    def test_typeddict(self):
        from typing import TypedDict
        class P(TypedDict):
            name: str
            age: int
        p = P(name="x", age=5)
        self.assertEqual(p["name"], "x")

    def test_generic_class(self):
        from typing import Generic, TypeVar
        T = TypeVar("T")
        class Box(Generic[T]):
            def __init__(self, v): self.v = v
        b = Box(5)
        self.assertEqual(b.v, 5)


unittest.main(globals())
