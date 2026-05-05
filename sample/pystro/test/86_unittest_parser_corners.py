import unittest


class ImplicitConcatTest(unittest.TestCase):
    def test_two_strs(self):
        self.assertEqual("foo" "bar", "foobar")

    def test_three_strs(self):
        self.assertEqual("a" " " "b", "a b")

    def test_bytes(self):
        self.assertEqual(b"foo" b"bar", b"foobar")

    def test_in_function_call(self):
        s = ("hello "
             "world")
        self.assertEqual(s, "hello world")


class MatchStarTest(unittest.TestCase):
    def test_list_star(self):
        def describe(x):
            match x:
                case [a, *rest]:
                    return (a, rest)
                case _:
                    return None
        self.assertEqual(describe([1, 2, 3, 4]), (1, [2, 3, 4]))
        self.assertEqual(describe([1]), (1, []))

    def test_list_star_middle(self):
        def f(x):
            match x:
                case [a, *mid, z]:
                    return (a, mid, z)
        self.assertEqual(f([1, 2, 3, 4, 5]), (1, [2, 3, 4], 5))


class ClassPatternTest(unittest.TestCase):
    def test_basic(self):
        class P:
            def __init__(self, x, y): self.x, self.y = x, y
        def what(p):
            match p:
                case P(x=0, y=0): return "origin"
                case P(x=x, y=0): return f"x={x}"
                case P(): return "other"
                case _: return "no"
        self.assertEqual(what(P(0, 0)), "origin")
        self.assertEqual(what(P(5, 0)), "x=5")
        self.assertEqual(what(P(3, 4)), "other")
        self.assertEqual(what(42), "no")


unittest.main(globals())
