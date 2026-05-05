import unittest


class StrPercentFormatTest(unittest.TestCase):
    def test_basic(self):
        self.assertEqual("%d + %s = %s" % (1, "two", 3), "1 + two = 3")

    def test_dict_form(self):
        self.assertEqual("%(name)s" % {"name": "X"}, "X")
        self.assertEqual("%(a)d/%(b)d" % {"a": 5, "b": 10}, "5/10")

    def test_dict_missing_key(self):
        with self.assertRaises(KeyError):
            "%(x)s" % {"y": 1}

    def test_width_precision(self):
        self.assertEqual("%5d" % 42, "   42")
        self.assertEqual("%-5d|" % 42, "42   |")
        self.assertEqual("%05d" % 42, "00042")
        self.assertEqual("%.3f" % 3.14159, "3.142")
        self.assertEqual("%8.3f" % 3.14, "   3.140")

    def test_dynamic_width(self):
        self.assertEqual("%*d" % (5, 42), "   42")

    def test_hex(self):
        self.assertEqual("%x" % 255, "ff")
        self.assertEqual("%#x" % 255, "0xff")
        self.assertEqual("%X" % 255, "FF")


class StrCountSliceTest(unittest.TestCase):
    def test_count_slice(self):
        s = "Hello, World!"
        self.assertEqual(s.count("l"), 3)
        self.assertEqual(s.count("l", 0, 5), 2)
        self.assertEqual(s.count("l", 4), 1)

    def test_count_empty(self):
        # Empty pattern: returns len+1 over the slice.
        self.assertEqual("abc".count(""), 4)


unittest.main(globals())
