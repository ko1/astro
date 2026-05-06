import unittest


class SplitNoneTest(unittest.TestCase):
    def test_explicit_none(self):
        self.assertEqual("  a  b  c  ".split(None), ["a", "b", "c"])

    def test_none_with_max(self):
        self.assertEqual("  a  b  c  ".split(None, 1), ["a", "b  c  "])

    def test_no_arg_same_as_none(self):
        self.assertEqual("a b c".split(), "a b c".split(None))


class FormatMapTest(unittest.TestCase):
    def test_with_dict(self):
        self.assertEqual("{a}-{b}".format_map({"a": 1, "b": 2}), "1-2")

    def test_with_user_mapping(self):
        class M:
            def __getitem__(self, k): return f"<{k}>"
        self.assertEqual("{name}".format_map(M()), "<name>")


class StrEdgeTest(unittest.TestCase):
    def test_split_same(self):
        self.assertEqual("ab".split("ab"), ["", ""])

    def test_count_non_overlapping(self):
        self.assertEqual("aaaa".count("aa"), 2)

    def test_isspace_empty(self):
        self.assertFalse("".isspace())

    def test_isalnum(self):
        self.assertTrue("abc123".isalnum())
        self.assertFalse("!".isalnum())

    def test_partition(self):
        self.assertEqual("a-b-c".partition("-"), ("a", "b-c"[:0] + "b-c", ""[0:0] + "")[:0] + ("a", "-", "b-c"))


unittest.main(globals())
