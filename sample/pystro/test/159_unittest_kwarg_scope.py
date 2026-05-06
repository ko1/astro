"""Verify kwargs in function calls don't shadow builtins as locals."""
import unittest


class KwargScopeTest(unittest.TestCase):
    def test_int_kwarg_doesnt_shadow(self):
        # The kwarg `type=int` should not register `int` as a local of this
        # method.  Previously pystro's prescan registered it (as None).
        def f(*, type=None): return type
        result = f(type=int)
        self.assertIs(result, int)
        self.assertEqual(int("42"), 42)

    def test_str_kwarg_doesnt_shadow(self):
        def f(*, str=None): return str
        result = f(str="hello")
        self.assertEqual(result, "hello")
        self.assertEqual(str(123), "123")

    def test_list_kwarg_doesnt_shadow(self):
        def f(*, list=None): return list
        result = f(list=[1, 2, 3])
        self.assertEqual(result, [1, 2, 3])
        self.assertEqual(list("abc"), ["a", "b", "c"])

    def test_walrus_still_works(self):
        # Walrus must still bind regardless of paren context.
        result = [y for x in range(3) if (y := x * 2) > 0]
        self.assertEqual(result, [2, 4])

    def test_nested_kwargs(self):
        # Multiple nested levels.
        def outer(**kw):
            return kw["x"]
        def inner(value=None):
            return value
        self.assertEqual(outer(x=inner(value=42)), 42)


unittest.main(globals())
