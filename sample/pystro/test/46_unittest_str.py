# Adapted from CPython test_str.py / test_unicode.py.

import unittest


class StrTest(unittest.TestCase):
    def test_constructor(self):
        self.assertEqual(str(), "")
        self.assertEqual(str(5), "5")
        self.assertEqual(str(3.14), "3.14")
        self.assertEqual(str(None), "None")
        self.assertEqual(str(True), "True")
        self.assertEqual(str([1, 2]), "[1, 2]")

    def test_concat(self):
        self.assertEqual("ab" + "cd", "abcd")
        self.assertEqual("a" * 3, "aaa")
        self.assertEqual(3 * "a", "aaa")
        self.assertEqual("" * 100, "")
        self.assertEqual("ab" * 0, "")

    def test_index(self):
        s = "hello"
        self.assertEqual(s[0], "h")
        self.assertEqual(s[-1], "o")
        self.assertEqual(s[2], "l")

    def test_slice(self):
        s = "abcdef"
        self.assertEqual(s[:3], "abc")
        self.assertEqual(s[3:], "def")
        self.assertEqual(s[1:4], "bcd")
        self.assertEqual(s[::2], "ace")
        self.assertEqual(s[::-1], "fedcba")
        self.assertEqual(s[1:5:2], "bd")
        self.assertEqual(s[100:], "")

    def test_membership(self):
        self.assertIn("ell", "hello")
        self.assertNotIn("xyz", "hello")
        self.assertIn("h", "hello")

    def test_split(self):
        self.assertEqual("a,b,c".split(","), ["a", "b", "c"])
        self.assertEqual("hello world".split(), ["hello", "world"])
        self.assertEqual("aXbXc".split("X", 1), ["a", "bXc"])

    def test_join(self):
        self.assertEqual(",".join(["a", "b", "c"]), "a,b,c")
        self.assertEqual("".join(["x", "y", "z"]), "xyz")

    def test_strip(self):
        self.assertEqual("  hi  ".strip(), "hi")
        self.assertEqual("---hi---".strip("-"), "hi")
        self.assertEqual("---hi---".lstrip("-"), "hi---")
        self.assertEqual("---hi---".rstrip("-"), "---hi")

    def test_case(self):
        self.assertEqual("Hello".upper(), "HELLO")
        self.assertEqual("Hello".lower(), "hello")
        self.assertEqual("hello world".title(), "Hello World")
        self.assertEqual("Hello".swapcase(), "hELLO")

    def test_starts_ends(self):
        self.assertTrue("hello".startswith("he"))
        self.assertFalse("hello".startswith("xy"))
        self.assertTrue("hello".endswith("lo"))
        self.assertFalse("hello".endswith("xy"))

    def test_find_replace(self):
        self.assertEqual("hello".find("ll"), 2)
        self.assertEqual("hello".find("xyz"), -1)
        self.assertEqual("hello".replace("l", "L"), "heLLo")
        self.assertEqual("hello".count("l"), 2)

    def test_zfill_just(self):
        self.assertEqual("42".zfill(5), "00042")
        self.assertEqual("hi".center(7, "-"), "--hi---")
        self.assertEqual("hi".ljust(5), "hi   ")
        self.assertEqual("hi".rjust(5), "   hi")

    def test_predicates(self):
        self.assertTrue("123".isdigit())
        self.assertFalse("12a".isdigit())
        self.assertTrue("abc".isalpha())
        self.assertFalse("a1".isalpha())
        self.assertTrue("abc123".isalnum())
        self.assertTrue("   ".isspace())

    def test_remove_prefix_suffix(self):
        self.assertEqual("hello".removeprefix("he"), "llo")
        self.assertEqual("hello".removeprefix("xx"), "hello")
        self.assertEqual("hello".removesuffix("lo"), "hel")
        self.assertEqual("hello".removesuffix("xx"), "hello")

    def test_partition(self):
        self.assertEqual("a,b,c".partition(","), ("a", ",", "b,c"))
        self.assertEqual("abc".partition("z"), ("abc", "", ""))

    def test_format(self):
        self.assertEqual("{} {}".format("a", "b"), "a b")
        self.assertEqual("{1} {0}".format("a", "b"), "b a")
        self.assertEqual("{:>5}".format("x"), "    x")
        self.assertEqual("{:0>3}".format(7), "007")

    def test_pct_format(self):
        self.assertEqual("%d" % 42, "42")
        self.assertEqual("%s %d" % ("x", 5), "x 5")
        self.assertEqual("%05d" % 42, "00042")
        self.assertEqual("%-10s|" % "hi", "hi        |")

    def test_iter(self):
        out = []
        for ch in "abc":
            out.append(ch)
        self.assertEqual(out, ["a", "b", "c"])

    def test_compare(self):
        self.assertTrue("a" < "b")
        self.assertTrue("abc" < "abd")
        self.assertEqual("abc", "abc")
        self.assertNotEqual("abc", "abd")

    def test_type(self):
        self.assertIs(type("x"), str)


unittest.main(globals())
