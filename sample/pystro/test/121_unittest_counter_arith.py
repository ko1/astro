import unittest
from collections import Counter


class CounterArithTest(unittest.TestCase):
    def test_add(self):
        c = Counter("aabb") + Counter("ab")
        self.assertEqual(c["a"], 3)
        self.assertEqual(c["b"], 3)

    def test_sub(self):
        c = Counter("aabb") - Counter("ab")
        self.assertEqual(c["a"], 1)
        self.assertEqual(c["b"], 1)

    def test_sub_drops_zero(self):
        c = Counter("ab") - Counter("ab")
        self.assertEqual(dict(c._d), {})

    def test_and(self):
        c = Counter("aabb") & Counter("aaa")
        self.assertEqual(c["a"], 2)
        self.assertEqual(c["b"], 0)

    def test_or(self):
        c = Counter("aabb") | Counter("aaa")
        self.assertEqual(c["a"], 3)
        self.assertEqual(c["b"], 2)


unittest.main(globals())
