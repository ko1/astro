"""Misc CPython compatibility additions: matmul, eval/exec args,
bytes.maketrans, typing.Generic, collections.abc isinstance, sys version."""
import unittest


class MatmulTest(unittest.TestCase):
    def test_user_matmul(self):
        class M:
            def __matmul__(self, o): return ("@", o)
        self.assertEqual(M() @ 5, ("@", 5))

    def test_rmatmul(self):
        class L:
            pass
        class R:
            def __rmatmul__(self, o): return "rmm"
        self.assertEqual(L() @ R(), "rmm")  # type: ignore

    def test_no_matmul_on_int(self):
        with self.assertRaises(TypeError):
            1 @ 2  # type: ignore


class EvalExecTest(unittest.TestCase):
    def test_eval_with_globals(self):
        self.assertEqual(eval("a + b", {"a": 1, "b": 2}), 3)

    def test_eval_no_globals(self):
        self.assertEqual(eval("1 + 2 * 3"), 7)

    def test_eval_compile(self):
        code = compile("x * 10", "<s>", "eval")
        self.assertEqual(eval(code, {"x": 5}), 50)

    def test_exec_basic(self):
        exec("zz = 99")
        self.assertEqual(zz, 99)


class BytesTranslateTest(unittest.TestCase):
    def test_maketrans(self):
        t = bytes.maketrans(b"ab", b"AB")
        self.assertEqual(b"banana".translate(t), b"BAnAnA")

    def test_translate_with_delete(self):
        t = bytes.maketrans(b"ab", b"AB")
        self.assertEqual(b"banana".translate(t, b"n"), b"BAAA")

    def test_translate_none(self):
        self.assertEqual(b"banana".translate(None, b"a"), b"bnn")


class TypingTest(unittest.TestCase):
    def test_generic(self):
        from typing import Generic, TypeVar
        T = TypeVar("T")
        class Box(Generic[T]):
            def __init__(self, x): self.x = x
        b = Box(5)
        self.assertEqual(b.x, 5)
        self.assertIs(Box[int], Box)

    def test_literal_annotated(self):
        from typing import Literal, Annotated
        # These are no-ops but should not raise on subscript.
        _ = Literal["a", "b"]
        _ = Annotated[int, "meta"]


class CollectionsAbcTest(unittest.TestCase):
    def test_iterable(self):
        from collections.abc import Iterable
        self.assertIsInstance([1, 2], Iterable)
        self.assertIsInstance("a", Iterable)
        self.assertNotIsInstance(42, Iterable)

    def test_mapping(self):
        from collections.abc import Mapping
        self.assertIsInstance({}, Mapping)
        self.assertNotIsInstance([], Mapping)

    def test_sequence(self):
        from collections.abc import Sequence
        self.assertIsInstance("abc", Sequence)
        self.assertIsInstance((1, 2), Sequence)
        self.assertNotIsInstance({1, 2}, Sequence)

    def test_callable(self):
        from collections.abc import Callable
        self.assertIsInstance(lambda: 1, Callable)
        self.assertNotIsInstance(42, Callable)


class SysVersionTest(unittest.TestCase):
    def test_version_info(self):
        import sys
        self.assertEqual(sys.version_info[0], 3)
        self.assertEqual(sys.version_info.major, 3)


class OsConstantsTest(unittest.TestCase):
    def test_sep_etc(self):
        import os
        self.assertEqual(os.sep, "/")
        self.assertEqual(os.linesep, "\n")
        self.assertEqual(os.curdir, ".")
        self.assertEqual(os.pardir, "..")


unittest.main(globals())
