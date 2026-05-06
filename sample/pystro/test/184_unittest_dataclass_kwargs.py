import unittest
from dataclasses import dataclass


class FrozenTest(unittest.TestCase):
    def test_assigning_raises(self):
        @dataclass(frozen=True)
        class F:
            x: int
        f = F(1)
        with self.assertRaises(AttributeError):
            f.x = 2

    def test_init_works(self):
        @dataclass(frozen=True)
        class F:
            x: int = 5
        self.assertEqual(F().x, 5)

    def test_hash_consistent(self):
        @dataclass(frozen=True)
        class F:
            x: int
        self.assertEqual(hash(F(1)), hash(F(1)))
        self.assertNotEqual(hash(F(1)), hash(F(2)))

    def test_set_dedup(self):
        @dataclass(frozen=True)
        class F:
            x: int
        self.assertEqual(len({F(1), F(1), F(2)}), 2)


class EqFalseTest(unittest.TestCase):
    def test_identity_eq(self):
        @dataclass(eq=False)
        class N:
            x: int
        a = N(1); b = N(1)
        self.assertFalse(a == b)
        self.assertTrue(a == a)


class OrderTest(unittest.TestCase):
    def test_order(self):
        @dataclass(order=True)
        class O:
            x: int
            y: int
        self.assertTrue(O(1, 2) < O(1, 3))
        self.assertTrue(O(1, 2) <= O(1, 2))
        self.assertFalse(O(2, 0) < O(1, 9))


unittest.main(globals())
