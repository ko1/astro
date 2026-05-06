import unittest


class StrMethodsTest(unittest.TestCase):
    def test_split_max(self):
        self.assertEqual("a,b,c,d".split(",", 2), ["a", "b", "c,d"])

    def test_split_default(self):
        self.assertEqual("  a  b  c  ".split(), ["a", "b", "c"])
        self.assertEqual("a\nb\tc\nd".split(), ["a", "b", "c", "d"])

    def test_rsplit(self):
        self.assertEqual("a,b,c,d".rsplit(",", 2), ["a,b", "c", "d"])

    def test_partition(self):
        self.assertEqual("a:b:c".partition(":"), ("a", ":", "b:c"))
        self.assertEqual("abc".partition(":"), ("abc", "", ""))
        self.assertEqual("a:b:c".rpartition(":"), ("a:b", ":", "c"))

    def test_expandtabs(self):
        self.assertEqual("a\tb".expandtabs(4), "a   b")
        self.assertEqual("a\tb".expandtabs(0), "ab")
        self.assertEqual("a\tb\tc".expandtabs(), "a       b       c")

    def test_zfill(self):
        self.assertEqual("42".zfill(5), "00042")
        self.assertEqual("-7".zfill(5), "-0007")
        self.assertEqual("+7".zfill(5), "+0007")

    def test_just(self):
        self.assertEqual("x".ljust(5, "-"), "x----")
        self.assertEqual("x".rjust(5, "-"), "----x")
        self.assertEqual("x".center(5, "-"), "--x--")

    def test_translate(self):
        table = str.maketrans({"a": "X", "b": "Y", "c": None})
        self.assertEqual("abcdef".translate(table), "XYdef")

    def test_count(self):
        self.assertEqual("abcabcabc".count("a"), 3)
        self.assertEqual("abcabcabc".count("a", 1, 7), 2)

    def test_find_with_start(self):
        self.assertEqual("abcabc".find("b"), 1)
        self.assertEqual("abcabc".find("b", 3), 4)

    def test_starts_ends_tuple(self):
        self.assertTrue("abc".startswith(("x", "a")))
        self.assertTrue("abc".endswith(("z", "c")))
        self.assertFalse("abc".startswith(("x", "z")))

    def test_removeprefix(self):
        self.assertEqual("foobar".removeprefix("foo"), "bar")
        self.assertEqual("foobar".removeprefix("xx"), "foobar")
        self.assertEqual("foobar".removesuffix("bar"), "foo")

    def test_isidentifier(self):
        self.assertTrue("hello".isidentifier())
        self.assertFalse("1ab".isidentifier())
        self.assertFalse("hi mom".isidentifier())

    def test_isascii(self):
        self.assertTrue("hello".isascii())
        self.assertFalse("éllo".isascii())

    def test_isprintable(self):
        self.assertTrue("hello".isprintable())
        self.assertFalse("hello\n".isprintable())


class StrFormatTest(unittest.TestCase):
    def test_format_positional(self):
        self.assertEqual("{} {}".format(1, 2), "1 2")
        self.assertEqual("{0} {1} {0}".format("a", "b"), "a b a")

    def test_format_kwarg(self):
        self.assertEqual("{name}".format(name="Alice"), "Alice")

    def test_format_index(self):
        self.assertEqual("{0[1]}".format(["a", "b", "c"]), "b")

    def test_format_conversion(self):
        class C:
            def __repr__(self): return "REPR"
            def __str__(self): return "STR"
        self.assertEqual("{!r}".format(C()), "REPR")
        self.assertEqual("{!s}".format(C()), "STR")

    def test_format_spec(self):
        self.assertEqual("{:>5}".format("hi"), "   hi")
        self.assertEqual("{:.2f}".format(3.14159), "3.14")
        self.assertEqual("{:_^10}".format("a"), "____a_____")


class PercentFormatTest(unittest.TestCase):
    def test_int(self):
        self.assertEqual("%d" % 42, "42")
        self.assertEqual("%5d" % 42, "   42")
        self.assertEqual("%05d" % 42, "00042")
        self.assertEqual("%+d" % 7, "+7")
        self.assertEqual("%-5d|" % 7, "7    |")

    def test_str(self):
        self.assertEqual("%s %s" % ("a", "b"), "a b")
        self.assertEqual("%-5s|" % "x", "x    |")

    def test_dict(self):
        self.assertEqual("%(name)s" % {"name": "X"}, "X")

    def test_float(self):
        self.assertEqual("%.2f" % 3.14159, "3.14")
        self.assertEqual("%5.2f" % 3.14159, " 3.14")

    def test_hex_oct_char(self):
        self.assertEqual("%x" % 255, "ff")
        self.assertEqual("%o" % 8, "10")
        self.assertEqual("%c" % 65, "A")

    def test_repr(self):
        self.assertEqual("%r" % "x", "'x'")

    def test_percent_literal(self):
        self.assertEqual("%%" % (), "%")


unittest.main(globals())
