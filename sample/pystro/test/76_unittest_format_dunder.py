import unittest


class FormatDunderTest(unittest.TestCase):
    def test_format_default(self):
        class M:
            def __format__(self, spec):
                return "M:" + spec
        m = M()
        self.assertEqual(f"{m}", "M:")
        self.assertEqual(f"{m:abc}", "M:abc")

    def test_nested_spec(self):
        spec = "08.2f"
        a = 1234.5
        self.assertEqual(f"{a:{spec}}", "01234.50")

    def test_nested_spec_complex(self):
        n = 5
        self.assertEqual(f"{42:0{n}d}", "00042")


class BuiltinSubclassMulTest(unittest.TestCase):
    def test_str_subclass_mul(self):
        class S(str):
            pass
        s = S("hi")
        self.assertEqual(s * 3, "hihi" + "hi")
        self.assertEqual(3 * s, "hihihi")

    def test_dict_subclass_in(self):
        class D(dict):
            pass
        d = D()
        d["a"] = 1
        self.assertIn("a", d)
        self.assertNotIn("b", d)


unittest.main(globals())
