import unittest
import re


class ReGroupsTest(unittest.TestCase):
    def test_match_groups(self):
        m = re.match(r"hello (\w+)", "hello world")
        self.assertIsNotNone(m)
        self.assertEqual(m.group(0), "hello world")
        self.assertEqual(m.group(1), "world")
        self.assertEqual(m.groups(), ("world",))

    def test_search(self):
        m = re.search(r"(\d+)-(\d+)", "x 12-34 y")
        self.assertEqual(m.groups(), ("12", "34"))
        self.assertEqual(m.span(), (2, 7))
        self.assertEqual(m.start(), 2)
        self.assertEqual(m.end(), 7)

    def test_findall(self):
        self.assertEqual(re.findall(r"\d+", "a1 b22 c333"), ["1", "22", "333"])
        self.assertEqual(re.findall(r"(\w+)=(\w+)", "a=1 b=2 c=3"),
                         [("a", "1"), ("b", "2"), ("c", "3")])

    def test_sub(self):
        self.assertEqual(re.sub(r"\d+", "X", "a1 b22 c333"), "aX bX cX")
        self.assertEqual(re.sub(r"(\w+)=(\w+)", r"\2=\1", "a=1 b=2"),
                         "1=a 2=b")

    def test_sub_callable(self):
        out = re.sub(r"\d+", lambda m: str(int(m.group()) * 2), "a1 b22")
        self.assertEqual(out, "a2 b44")

    def test_split(self):
        self.assertEqual(re.split(r"\s+", "a b  c   d"), ["a", "b", "c", "d"])

    def test_compile(self):
        p = re.compile(r"(\w+):(\w+)")
        m = p.search("name:Alice age:30")
        self.assertEqual(m.groups(), ("name", "Alice"))

    def test_ignorecase(self):
        m = re.match(r"hello", "HELLO", re.IGNORECASE)
        self.assertEqual(m.group(), "HELLO")
        self.assertIsNone(re.match(r"hello", "HELLO"))

    def test_fullmatch(self):
        self.assertIsNotNone(re.fullmatch(r"\d+", "12345"))
        self.assertIsNone(re.fullmatch(r"\d+", "12345abc"))

    def test_finditer(self):
        out = [m.group() for m in re.finditer(r"\d+", "a1 b22 c333")]
        self.assertEqual(out, ["1", "22", "333"])

    def test_escape(self):
        self.assertEqual(re.escape("a.b*c"), r"a\.b\*c")

    def test_charclass(self):
        m = re.match(r"[a-zA-Z]+", "Hello123")
        self.assertEqual(m.group(), "Hello")

    def test_negated_charclass(self):
        m = re.match(r"[^0-9]+", "abc123")
        self.assertEqual(m.group(), "abc")

    def test_quantifier_question(self):
        self.assertEqual(re.match(r"colou?r", "color").group(), "color")
        self.assertEqual(re.match(r"colou?r", "colour").group(), "colour")

    def test_anchors(self):
        self.assertIsNotNone(re.match(r"^hello", "hello world"))
        self.assertIsNotNone(re.match(r"hello$", "hello"))
        self.assertIsNone(re.match(r"^world", "hello world"))


unittest.main(globals())
