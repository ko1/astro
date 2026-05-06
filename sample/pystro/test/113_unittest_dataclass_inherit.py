import unittest
from dataclasses import dataclass


class DataclassInheritTest(unittest.TestCase):
    def test_basic(self):
        @dataclass
        class Base:
            a: int

        @dataclass
        class Derived(Base):
            b: int = 0

        d = Derived(1, 2)
        self.assertEqual(d.a, 1)
        self.assertEqual(d.b, 2)

    def test_default_in_inherited(self):
        @dataclass
        class B:
            x: int = 10

        @dataclass
        class D(B):
            y: int = 20

        self.assertEqual(D().x, 10)
        self.assertEqual(D(99).x, 99)
        self.assertEqual(D(1, 2).y, 2)

    def test_post_init(self):
        @dataclass
        class P:
            x: int
            y: int
            def __post_init__(self):
                self.dist = (self.x ** 2 + self.y ** 2) ** 0.5

        p = P(3, 4)
        self.assertEqual(p.dist, 5.0)


unittest.main(globals())
