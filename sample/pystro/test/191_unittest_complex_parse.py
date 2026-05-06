import unittest


class ComplexParseTest(unittest.TestCase):
    def test_real_imag(self):
        self.assertEqual(complex("1+2j"), complex(1, 2))

    def test_negative_real(self):
        self.assertEqual(complex("-1+2j"), complex(-1, 2))

    def test_negative_imag(self):
        self.assertEqual(complex("1-2j"), complex(1, -2))

    def test_pure_imag(self):
        self.assertEqual(complex("1.5j"), complex(0, 1.5))
        self.assertEqual(complex("j"), complex(0, 1))
        self.assertEqual(complex("-j"), complex(0, -1))

    def test_pure_real(self):
        self.assertEqual(complex("3.14"), complex(3.14, 0))

    def test_paren_form(self):
        self.assertEqual(complex("(1+2j)"), complex(1, 2))


class IntComplexAttrTest(unittest.TestCase):
    def test_int_real_imag_property(self):
        x = 5
        self.assertEqual(x.real, 5)
        self.assertEqual(x.imag, 0)

    def test_int_numerator_denominator(self):
        self.assertEqual((7).numerator, 7)
        self.assertEqual((7).denominator, 1)

    def test_float_real_imag(self):
        self.assertEqual((3.14).real, 3.14)
        self.assertEqual((3.14).imag, 0.0)

    def test_complex_real_imag(self):
        c = complex(1, 2)
        self.assertEqual(c.real, 1.0)
        self.assertEqual(c.imag, 2.0)

    def test_int_conjugate(self):
        self.assertEqual((5).conjugate(), 5)


unittest.main(globals())
