import unittest


class MatchBuiltinTypeTest(unittest.TestCase):
    def test_int_str_float(self):
        def classify(x):
            match x:
                case int(): return "int"
                case str(): return "str"
                case float(): return "float"
                case _: return "other"
        self.assertEqual(classify(1), "int")
        self.assertEqual(classify("a"), "str")
        self.assertEqual(classify(3.14), "float")
        self.assertEqual(classify([1]), "other")

    def test_with_guard(self):
        def f(x):
            match x:
                case int() if x > 0: return "pos"
                case int() if x < 0: return "neg"
                case 0: return "zero"
                case _: return "other"
        self.assertEqual(f(5), "pos")
        self.assertEqual(f(-1), "neg")
        self.assertEqual(f(0), "zero")
        self.assertEqual(f("hi"), "other")

    def test_list_class(self):
        def f(x):
            match x:
                case list(): return "list"
                case dict(): return "dict"
                case _: return "other"
        self.assertEqual(f([1, 2]), "list")
        self.assertEqual(f({"a": 1}), "dict")
        self.assertEqual(f("x"), "other")


unittest.main(globals())
