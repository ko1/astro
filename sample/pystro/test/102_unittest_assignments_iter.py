import unittest


class TrailingCommaTupleTest(unittest.TestCase):
    def test_one_tuple(self):
        t = 1,
        self.assertEqual(t, (1,))
        self.assertEqual(len(t), 1)

    def test_function_def_trailing(self):
        def f(a, b, c,):
            return a + b + c
        self.assertEqual(f(1, 2, 3,), 6)


class ListIaddIterTest(unittest.TestCase):
    def test_iter(self):
        xs = [1, 2]
        xs += iter([3, 4])
        self.assertEqual(xs, [1, 2, 3, 4])

    def test_genexp(self):
        xs = [1]
        xs += (x*x for x in range(3))
        self.assertEqual(xs, [1, 0, 1, 4])


unittest.main(globals())
