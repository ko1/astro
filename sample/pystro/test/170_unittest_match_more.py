import unittest


class MatchLiteralTest(unittest.TestCase):
    def test_int_str(self):
        def f(v):
            match v:
                case 1: return "one"
                case "hello": return "hi"
                case _: return "other"
        self.assertEqual(f(1), "one")
        self.assertEqual(f("hello"), "hi")
        self.assertEqual(f("x"), "other")


class MatchSequenceTest(unittest.TestCase):
    def test_list_patterns(self):
        def f(v):
            match v:
                case []: return "empty"
                case [x]: return f"one:{x}"
                case [x, y]: return f"two:{x},{y}"
                case [x, *rest]: return f"head:{x},rest:{rest}"
                case _: return "?"
        self.assertEqual(f([]), "empty")
        self.assertEqual(f([1]), "one:1")
        self.assertEqual(f([1, 2]), "two:1,2")
        self.assertEqual(f([1, 2, 3]), "head:1,rest:[2, 3]")


class MatchClassKwargTest(unittest.TestCase):
    def test_kwarg_form(self):
        class P:
            def __init__(self, x, y): self.x = x; self.y = y
        def m(v):
            match v:
                case P(x=0, y=0): return "origin"
                case P(x=0, y=y): return f"y={y}"
                case P(x=x, y=0): return f"x={x}"
                case P(x=x, y=y): return f"{x},{y}"
        self.assertEqual(m(P(0, 0)), "origin")
        self.assertEqual(m(P(0, 5)), "y=5")
        self.assertEqual(m(P(3, 0)), "x=3")
        self.assertEqual(m(P(2, 3)), "2,3")


class MatchMappingTest(unittest.TestCase):
    def test_dict_pattern(self):
        def f(d):
            match d:
                case {"name": n, "age": a}: return f"{n}:{a}"
                case {"name": n}: return n
                case _: return "?"
        self.assertEqual(f({"name": "A", "age": 30}), "A:30")
        self.assertEqual(f({"name": "B"}), "B")
        self.assertEqual(f({}), "?")


class MatchOrAndGuardTest(unittest.TestCase):
    def test_or(self):
        def f(v):
            match v:
                case "red" | "blue" | "green": return "primary"
                case _: return "other"
        self.assertEqual(f("red"), "primary")
        self.assertEqual(f("yellow"), "other")

    def test_guard(self):
        def f(n):
            match n:
                case x if x > 0: return "pos"
                case x if x < 0: return "neg"
                case 0: return "zero"
        self.assertEqual(f(5), "pos")
        self.assertEqual(f(-3), "neg")
        self.assertEqual(f(0), "zero")

    def test_walrus_in_guard(self):
        def f(seq):
            match seq:
                case [x, y] if (s := x + y) > 5:
                    return f"sum {s}"
                case _: return "small"
        self.assertEqual(f([3, 5]), "sum 8")
        self.assertEqual(f([1, 2]), "small")


unittest.main(globals())
