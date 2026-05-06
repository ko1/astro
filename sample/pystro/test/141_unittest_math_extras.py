import unittest
import math


class MathExtrasTest(unittest.TestCase):
    def test_dist(self):
        self.assertEqual(math.dist((0, 0), (3, 4)), 5.0)
        self.assertEqual(math.dist([1, 2, 3], [4, 6, 3]), 5.0)
        with self.assertRaises(ValueError):
            math.dist((1, 2), (1, 2, 3))

    def test_hypot_n(self):
        self.assertEqual(math.hypot(3, 4), 5.0)
        self.assertEqual(math.hypot(1, 2, 2), 3.0)
        self.assertEqual(math.hypot(), 0.0)

    def test_fsum(self):
        self.assertTrue(abs(math.fsum([0.1] * 10) - 1.0) < 1e-9)
        self.assertEqual(math.fsum([]), 0.0)

    def test_expm1_log1p(self):
        self.assertEqual(math.expm1(0), 0.0)
        self.assertEqual(math.log1p(0), 0.0)
        self.assertTrue(abs(math.expm1(1) - (math.e - 1)) < 1e-9)

    def test_cbrt(self):
        self.assertEqual(math.cbrt(27), 3.0)
        self.assertEqual(math.cbrt(-8), -2.0)
        self.assertEqual(math.cbrt(0), 0.0)


unittest.main(globals())
