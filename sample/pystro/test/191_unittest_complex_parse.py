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
    def test_int_real_imag_callable(self):
        # In pystro, int.real / int.imag are methods (not properties).
        x = 5
        self.assertEqual(x.real(), 5)
        self.assertEqual(x.imag(), 0)

    def test_int_conjugate(self):
        self.assertEqual((5).conjugate(), 5)


unittest.main(globals())
