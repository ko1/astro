# Adapted from CPython test_funcattrs.py / test_call.py.

import unittest


def free_fn(x): return x * 2


class FuncTest(unittest.TestCase):
    def test_basic(self):
        self.assertEqual(free_fn(5), 10)

    def test_lambda(self):
        f = lambda x: x + 1
        self.assertEqual(f(5), 6)

    def test_closure(self):
        def make_adder(n):
            def add(x):
                return x + n
            return add
        a = make_adder(10)
        self.assertEqual(a(5), 15)

    def test_default_args(self):
        def f(x, y=10):
            return x + y
        self.assertEqual(f(5), 15)
        self.assertEqual(f(5, 100), 105)

    def test_starargs(self):
        def f(*args):
            return list(args)
        self.assertEqual(f(), [])
        self.assertEqual(f(1, 2, 3), [1, 2, 3])

    def test_kwargs(self):
        def f(**kw):
            return sorted(kw.keys())
        self.assertEqual(f(), [])
        self.assertEqual(f(a=1, b=2), ["a", "b"])

    def test_mixed(self):
        def f(a, b, *args, c=99, **kw):
            return [a, b, list(args), c, sorted(kw.keys())]
        self.assertEqual(f(1, 2), [1, 2, [], 99, []])
        self.assertEqual(f(1, 2, 3, 4, 5), [1, 2, [3, 4, 5], 99, []])
        self.assertEqual(f(1, 2, 3, c=10, x=100), [1, 2, [3], 10, ["x"]])

    def test_spread_call(self):
        def f(a, b, c):
            return a + b + c
        args = [1, 2, 3]
        self.assertEqual(f(*args), 6)
        kw = {"a": 1, "b": 2, "c": 3}
        self.assertEqual(f(**kw), 6)

    def test_func_attr(self):
        def f(): pass
        f.x = 99
        self.assertEqual(f.x, 99)
        self.assertEqual(f.__name__, "f")

    def test_decorator(self):
        def deco(fn):
            def w(*a):
                return fn(*a) + 1
            return w
        @deco
        def f(x): return x * 2
        self.assertEqual(f(5), 11)

    def test_decorator_factory(self):
        def repeat(n):
            def deco(fn):
                def w(*a):
                    r = None
                    for _ in range(n):
                        r = fn(*a)
                    return r
                return w
            return deco
        @repeat(3)
        def f(): return 42
        self.assertEqual(f(), 42)

    def test_lambda_factory(self):
        def make(i):
            return lambda: i
        fs = [make(i) for i in range(3)]
        self.assertEqual([f() for f in fs], [0, 1, 2])

    def test_nested_def(self):
        def outer():
            def inner():
                return 99
            return inner
        self.assertEqual(outer()(), 99)

    def test_global(self):
        global FRT_GLOBAL
        FRT_GLOBAL = 0
        def bump():
            global FRT_GLOBAL
            FRT_GLOBAL += 1
        bump(); bump(); bump()
        self.assertEqual(FRT_GLOBAL, 3)

    def test_nonlocal(self):
        def outer():
            n = 0
            def inc():
                nonlocal n
                n += 1
                return n
            return inc
        f = outer()
        self.assertEqual(f(), 1)
        self.assertEqual(f(), 2)
        self.assertEqual(f(), 3)

    def test_annotation(self):
        def f(x: int, y: str = "hello") -> int:
            return x
        self.assertEqual(f(5), 5)


unittest.main(globals())
