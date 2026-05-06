import unittest


class SliceObjectTest(unittest.TestCase):
    def test_basic(self):
        s = slice(1, 5)
        self.assertEqual(s.start, 1)
        self.assertEqual(s.stop, 5)
        self.assertIsNone(s.step)

    def test_index_via_slice(self):
        xs = list(range(10))
        s = slice(2, 8, 2)
        self.assertEqual(xs[s], [2, 4, 6])

    def test_str_index(self):
        s = "hello world"
        sl = slice(6, None)
        self.assertEqual(s[sl], "world")

    def test_neg_step(self):
        xs = [1, 2, 3, 4, 5]
        self.assertEqual(xs[slice(None, None, -1)], [5, 4, 3, 2, 1])


unittest.main(globals())
