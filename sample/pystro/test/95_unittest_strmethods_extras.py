import unittest


class StartsEndsRangeTest(unittest.TestCase):
    def test_starts_at(self):
        s = "abcdefg"
        self.assertTrue(s.startswith("cde", 2))
        self.assertFalse(s.startswith("cde", 3))
        self.assertTrue(s.startswith("a", 0, 1))
        self.assertFalse(s.startswith("a", 0, 0))

    def test_ends_at(self):
        s = "abcdefg"
        self.assertTrue(s.endswith("cd", 0, 4))
        self.assertFalse(s.endswith("cd", 0, 5))
        self.assertTrue(s.endswith("g"))


class BuiltinAttrTest(unittest.TestCase):
    def test_class(self):
        f = str.upper
        self.assertEqual(f.__class__.__name__, "builtin_function_or_method")

    def test_name(self):
        self.assertEqual(str.upper.__name__, "upper")

    def test_obj_class(self):
        self.assertEqual((1).__class__, int)
        self.assertEqual("a".__class__, str)
        self.assertEqual([].__class__, list)
        self.assertEqual({}.__class__, dict)


unittest.main(globals())
