import unittest


class FloatExtrasTest(unittest.TestCase):
    def test_as_integer_ratio_half(self):
        self.assertEqual((0.5).as_integer_ratio(), (1, 2))

    def test_as_integer_ratio_quarter(self):
        self.assertEqual((0.25).as_integer_ratio(), (1, 4))

    def test_as_integer_ratio_neg(self):
        self.assertEqual((-0.5).as_integer_ratio(), (-1, 2))

    def test_as_integer_ratio_int(self):
        self.assertEqual((3.0).as_integer_ratio(), (3, 1))

    def test_hex_roundtrip(self):
        for x in [0.1, 1.0, 3.14, -2.5, 1e-10]:
            h = x.hex()
            self.assertEqual(float.fromhex(h), x)


class BigIntSliceTest(unittest.TestCase):
    def test_huge_slice_start(self):
        xs = [1, 2, 3]
        self.assertEqual(xs[10**100:], [])

    def test_huge_slice_stop(self):
        xs = [1, 2, 3]
        self.assertEqual(xs[:10**100], [1, 2, 3])

    def test_neg_huge_slice(self):
        xs = [1, 2, 3]
        self.assertEqual(xs[-10**100:], [1, 2, 3])


class NaNIdentityTest(unittest.TestCase):
    def test_nan_in_self(self):
        n = float("nan")
        self.assertIn(n, [n])  # identity short-circuit

    def test_nan_distinct(self):
        # New NaN each call — no identity match.
        self.assertNotIn(float("nan"), [float("nan")])


unittest.main(globals())
