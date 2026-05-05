import unittest
import abc
import math


class AbcEnforcementTest(unittest.TestCase):
    def test_abstract_cannot_instantiate(self):
        class Shape(abc.ABC):
            @abc.abstractmethod
            def area(self): pass
        with self.assertRaises(TypeError):
            Shape()

    def test_concrete_subclass_works(self):
        class Shape(abc.ABC):
            @abc.abstractmethod
            def area(self): pass
        class Square(Shape):
            def __init__(self, n): self.n = n
            def area(self): return self.n * self.n
        self.assertEqual(Square(5).area(), 25)

    def test_partial_implementation_fails(self):
        class A(abc.ABC):
            @abc.abstractmethod
            def f(self): pass
            @abc.abstractmethod
            def g(self): pass
        class B(A):
            def f(self): return 1
            # g still abstract
        with self.assertRaises(TypeError):
            B()


class ComplexPowTest(unittest.TestCase):
    def test_basic_pow(self):
        z = 1 + 0j
        self.assertEqual(z ** 2, 1 + 0j)

    def test_imag_pow(self):
        z = 0 + 1j  # i
        # i^2 = -1
        r = z ** 2
        self.assertAlmostEqual(r.real, -1.0, places=10)
        self.assertAlmostEqual(r.imag, 0.0, places=10)

    def test_real_complex_mixed(self):
        # 2 ** (1+0j) = 2
        r = 2 ** (1 + 0j)
        self.assertAlmostEqual(r.real, 2.0, places=10)


unittest.main(globals())
