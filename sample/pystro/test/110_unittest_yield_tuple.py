import unittest


class YieldTupleTest(unittest.TestCase):
    def test_basic(self):
        def f():
            yield 1, 2
            yield 3, 4
        self.assertEqual(list(f()), [(1, 2), (3, 4)])

    def test_my_enumerate(self):
        def my_enumerate(iterable, start=0):
            i = start
            for x in iterable:
                yield i, x
                i += 1
        self.assertEqual(list(my_enumerate(["a", "b"], 10)),
                         [(10, "a"), (11, "b")])


unittest.main(globals())
