import unittest


class FloatLiteralFormsTest(unittest.TestCase):
    def test_leading_dot(self):
        self.assertEqual(.5, 0.5)
        self.assertEqual(.25, 0.25)

    def test_trailing_dot(self):
        self.assertEqual(5., 5.0)
        self.assertEqual(100., 100.0)


class NegBasePowTest(unittest.TestCase):
    def test_int_neg_base(self):
        # (-2)**0.5 → complex (CPython behaviour).
        r = (-2) ** 0.5
        self.assertAlmostEqual(r.imag, 1.41421356, places=5)
        self.assertAlmostEqual(r.real, 0, places=5)

    def test_int_neg_base_int_exp(self):
        # Integer exponent: still real result.
        self.assertEqual((-2) ** 3, -8)


unittest.main(globals())
