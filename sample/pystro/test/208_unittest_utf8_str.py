"""UTF-8 codepoint-aware string operations."""
import unittest


class UTF8StrTest(unittest.TestCase):
    def test_len_codepoint(self):
        self.assertEqual(len("héllo"), 5)
        self.assertEqual(len("café"), 4)
        self.assertEqual(len("日本語"), 3)
        self.assertEqual(len(""), 0)

    def test_indexing(self):
        s = "héllo"
        self.assertEqual(s[0], "h")
        self.assertEqual(s[1], "é")
        self.assertEqual(s[2], "l")
        self.assertEqual(s[-1], "o")
        self.assertEqual(s[-5], "h")

    def test_slice_step1(self):
        s = "héllo wörld"
        self.assertEqual(s[1:4], "éll")
        self.assertEqual(s[6:9], "wör")
        self.assertEqual(s[:5], "héllo")
        self.assertEqual(s[6:], "wörld")
        self.assertEqual(s[:], "héllo wörld")

    def test_slice_negative(self):
        s = "héllo"
        self.assertEqual(s[::-1], "olléh")
        self.assertEqual(s[::2], "hlo")
        self.assertEqual(s[-3:], "llo")

    def test_iteration(self):
        cps = []
        for ch in "héllo":
            cps.append(ch)
        self.assertEqual(cps, ["h", "é", "l", "l", "o"])

    def test_iteration_japanese(self):
        chars = list("日本語")
        self.assertEqual(chars, ["日", "本", "語"])
        self.assertEqual(len(chars), 3)

    def test_ord_chr_roundtrip(self):
        for s in ("é", "ñ", "日"):
            self.assertEqual(chr(ord(s)), s)

    def test_find_codepoint(self):
        s = "héllo wörld"
        self.assertEqual(s.find("ö"), 7)
        self.assertEqual(s.find("l"), 2)
        self.assertEqual(s.find("z"), -1)
        self.assertEqual(s.find("é"), 1)

    def test_rfind_codepoint(self):
        s = "héllo wörld"
        self.assertEqual(s.rfind("l"), 9)
        self.assertEqual(s.rfind("ö"), 7)

    def test_index_raises(self):
        with self.assertRaises(ValueError):
            "héllo".index("z")

    def test_count(self):
        self.assertEqual("héllo".count("l"), 2)
        self.assertEqual("éééhello".count("é"), 3)

    def test_replace(self):
        self.assertEqual("héllo".replace("é", "e"), "hello")
        self.assertEqual("ééé".replace("é", "x"), "xxx")

    def test_strip_codepoint(self):
        self.assertEqual("éééhelloéé".strip("é"), "hello")
        self.assertEqual("xöxhelloxöx".strip("xö"), "hello")
        self.assertEqual("éhelloé".lstrip("é"), "helloé")
        self.assertEqual("éhelloé".rstrip("é"), "éhello")

    def test_startswith_endswith(self):
        s = "héllo wörld"
        self.assertTrue(s.startswith("hé"))
        self.assertTrue(s.startswith("é", 1))
        self.assertTrue(s.endswith("ld"))
        self.assertTrue(s.endswith("ör", 0, 9))

    def test_ljust_rjust_center_codepoint(self):
        self.assertEqual("é".center(5, "*"), "**é**")
        self.assertEqual("hé".ljust(5, "x"), "héxxx")
        self.assertEqual("hé".rjust(5), "   hé")
        self.assertEqual("é".center(5, "ö"), "ööéöö")

    def test_zfill(self):
        self.assertEqual("4".zfill(5), "00004")
        self.assertEqual("-7".zfill(5), "-0007")

    def test_in_operator(self):
        self.assertTrue("é" in "héllo")
        self.assertTrue("ö" in "wörld")
        self.assertFalse("ñ" in "wörld")

    def test_concat_repeat(self):
        self.assertEqual("é" + "ö", "éö")
        self.assertEqual("é" * 3, "ééé")
        self.assertEqual(len("é" * 3), 3)

    def test_split(self):
        self.assertEqual("a,é,ö,b".split(","), ["a", "é", "ö", "b"])
        self.assertEqual("a éb".split(), ["a", "éb"])

    def test_join(self):
        self.assertEqual("é".join(["a", "b", "c"]), "aébéc")
        self.assertEqual("".join("héllo"), "héllo")

    def test_min_max(self):
        # min/max yield codepoint strings.
        self.assertEqual(min("héllo"), "h")
        self.assertEqual(max("abc"), "c")

    def test_sorted(self):
        self.assertEqual(sorted("écbéa"), ["a", "b", "c", "é", "é"])

    def test_reversed(self):
        self.assertEqual("".join(reversed("héllo")), "olléh")

    def test_unicode_escape(self):
        self.assertEqual("é", "é")
        self.assertEqual(len("é"), 1)
        self.assertEqual(ord("é"), 233)


unittest.main(globals())
