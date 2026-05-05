# Adapted from CPython test_complex.py.

import unittest


class ComplexTest(unittest.TestCase):
    def test_literal(self):
        z = 2+3j
        self.assertEqual(z.real, 2.0)
        self.assertEqual(z.imag, 3.0)
        c = 1j
        self.assertEqual(c, complex(0, 1))
        self.assertEqual(c.real, 0.0)
        self.assertEqual(c.imag, 1.0)

    def test_constructor(self):
        self.assertEqual(complex(), 0)
        self.assertEqual(complex(5), 5+0j)
        self.assertEqual(complex(3, 4), 3+4j)
        self.assertEqual(complex(1+2j), 1+2j)

    def test_add(self):
        self.assertEqual((1+2j) + (3+4j), 4+6j)
        self.assertEqual(1 + 2j, 1+2j)
        self.assertEqual(2j + 1, 1+2j)

    def test_sub(self):
        self.assertEqual((5+5j) - (1+2j), 4+3j)
        self.assertEqual(5 - 2j, 5-2j)

    def test_mul(self):
        self.assertEqual((1+2j) * (3+4j), -5+10j)
        self.assertEqual(2j * 3, 6j)
        self.assertEqual(1j * 1j, -1+0j)

    def test_div(self):
        self.assertEqual((1+1j) / (1-1j), 1j)

    def test_neg(self):
        self.assertEqual(-(1+2j), -1-2j)
        self.assertEqual(-1j, complex(0, -1))

    def test_eq(self):
        self.assertEqual(1+2j, 1+2j)
        self.assertNotEqual(1+2j, 1+3j)
        self.assertEqual(0+0j, 0)
        self.assertEqual(5+0j, 5)

    def test_type(self):
        self.assertIs(type(1j), complex)
        self.assertIsInstance(1j, complex)

    def test_zerodiv(self):
        try:
            x = (1+1j) / 0
            self.fail("expected ZeroDivisionError")
        except ZeroDivisionError:
            pass

    def test_repr(self):
        self.assertEqual(repr(1j), "1j")
        self.assertEqual(repr(2+3j), "(2+3j)")
        self.assertEqual(repr(2-3j), "(2-3j)")


unittest.main(globals())
