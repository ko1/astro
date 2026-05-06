import unittest


class GenexpIteratorTest(unittest.TestCase):
    def test_next(self):
        g = (x*x for x in range(5))
        self.assertEqual(next(g), 0)
        self.assertEqual(next(g), 1)
        self.assertEqual(next(g), 4)

    def test_exhausted(self):
        g = (x for x in range(2))
        next(g); next(g)
        with self.assertRaises(StopIteration):
            next(g)

    def test_to_list(self):
        self.assertEqual(list(x*2 for x in range(3)), [0, 2, 4])

    def test_in_sum(self):
        self.assertEqual(sum(x for x in range(5)), 10)

    def test_in_for_loop(self):
        total = 0
        for v in (x*x for x in range(5)):
            total += v
        self.assertEqual(total, 30)

    def test_filter(self):
        evens = (x for x in range(10) if x % 2 == 0)
        self.assertEqual(list(evens), [0, 2, 4, 6, 8])


unittest.main(globals())
