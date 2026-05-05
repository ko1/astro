import unittest


class DelSliceTest(unittest.TestCase):
    def test_del_basic(self):
        L = [1, 2, 3, 4, 5]
        del L[1:3]
        self.assertEqual(L, [1, 4, 5])

    def test_del_step(self):
        L = [1, 2, 3, 4, 5]
        del L[::2]
        self.assertEqual(L, [2, 4])

    def test_del_neg_step(self):
        L = [1, 2, 3, 4, 5]
        del L[::-2]
        self.assertEqual(L, [2, 4])


class StepSliceAssignTest(unittest.TestCase):
    def test_step_assign_match(self):
        L = [1, 2, 3, 4, 5]
        L[::2] = [10, 20, 30]
        self.assertEqual(L, [10, 2, 20, 4, 30])

    def test_step_assign_mismatch(self):
        L = [1, 2, 3, 4, 5]
        with self.assertRaises(ValueError):
            L[::2] = [10, 20]


class RangeEqTest(unittest.TestCase):
    def test_eq_same(self):
        self.assertEqual(range(5), range(5))
        self.assertEqual(range(0, 5), range(0, 5, 1))

    def test_eq_empty(self):
        # Empty ranges equal regardless of start/step.
        self.assertEqual(range(0), range(5, 0))
        self.assertEqual(range(0, 0, 5), range(10, 5))

    def test_eq_single(self):
        # Single-element ranges with same value but different step.
        self.assertEqual(range(0, 1), range(0, 1, 99))


class ZipStrictTest(unittest.TestCase):
    def test_strict_equal_lengths(self):
        self.assertEqual(list(zip([1, 2], [3, 4], strict=True)),
                         [(1, 3), (2, 4)])

    def test_strict_longer_second(self):
        with self.assertRaises(ValueError):
            list(zip([1, 2], [3, 4, 5], strict=True))

    def test_strict_longer_first(self):
        with self.assertRaises(ValueError):
            list(zip([1, 2, 3], [4, 5], strict=True))

    def test_non_strict_truncates(self):
        self.assertEqual(list(zip([1, 2, 3], [4, 5])),
                         [(1, 4), (2, 5)])


class StarUnpackTest(unittest.TestCase):
    def test_left(self):
        a, *b = [1, 2, 3, 4]
        self.assertEqual(a, 1)
        self.assertEqual(b, [2, 3, 4])

    def test_right(self):
        *a, b = [1, 2, 3, 4]
        self.assertEqual(a, [1, 2, 3])
        self.assertEqual(b, 4)

    def test_middle(self):
        a, *b, c = [1, 2, 3, 4, 5]
        self.assertEqual(a, 1)
        self.assertEqual(b, [2, 3, 4])
        self.assertEqual(c, 5)


unittest.main(globals())
