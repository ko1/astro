import unittest


class LambdaVarargsTest(unittest.TestCase):
    def test_args(self):
        f = lambda *args: args
        self.assertEqual(f(), ())
        self.assertEqual(f(1), (1,))
        self.assertEqual(f(1, 2, 3), (1, 2, 3))

    def test_args_in_body(self):
        s = lambda *args: sum(args)
        self.assertEqual(s(1, 2, 3, 4, 5), 15)

    def test_kwargs(self):
        f = lambda **kw: kw
        self.assertEqual(f(), {})
        self.assertEqual(f(a=1, b=2), {"a": 1, "b": 2})

    def test_mixed(self):
        f = lambda x, *args, **kw: (x, args, kw)
        self.assertEqual(f(1), (1, (), {}))
        self.assertEqual(f(1, 2, 3, k=4), (1, (2, 3), {"k": 4}))

    def test_kwonly(self):
        f = lambda x, *, k=10: x + k
        self.assertEqual(f(5), 15)
        self.assertEqual(f(5, k=20), 25)


unittest.main(globals())
