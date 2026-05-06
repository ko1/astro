import unittest


class FormatGroupingTest(unittest.TestCase):
    def test_int_comma(self):
        self.assertEqual("{:,d}".format(1234567), "1,234,567")

    def test_int_underscore(self):
        self.assertEqual("{:_d}".format(1234567), "1_234_567")

    def test_float_comma(self):
        self.assertEqual("{:,.2f}".format(1234567.891), "1,234,567.89")

    def test_neg_float_comma(self):
        self.assertEqual("{:,.0f}".format(-1234567.5), "-1,234,568")

    def test_no_grouping_below_1000(self):
        self.assertEqual("{:,d}".format(999), "999")


class IterIdempotentTest(unittest.TestCase):
    def test_iter_iter(self):
        xs = [1, 2, 3]
        it1 = iter(xs)
        it2 = iter(it1)
        self.assertIs(it1, it2)


unittest.main(globals())
