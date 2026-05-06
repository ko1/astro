import unittest


class MatchArgsTest(unittest.TestCase):
    def test_positional(self):
        class P:
            __match_args__ = ("x", "y")
            def __init__(self, x, y): self.x = x; self.y = y
        def m(p):
            match p:
                case P(0, 0): return "origin"
                case P(0, y): return ("y-axis", y)
                case P(x, 0): return ("x-axis", x)
                case P(x, y): return (x, y)
        self.assertEqual(m(P(0, 0)), "origin")
        self.assertEqual(m(P(0, 5)), ("y-axis", 5))
        self.assertEqual(m(P(3, 0)), ("x-axis", 3))
        self.assertEqual(m(P(2, 3)), (2, 3))

    def test_mixed_positional_keyword(self):
        class P:
            __match_args__ = ("a", "b")
            def __init__(self, a, b, c=0):
                self.a = a; self.b = b; self.c = c
        def m(p):
            match p:
                case P(1, 2, c=99): return "matched"
                case _: return "no"
        self.assertEqual(m(P(1, 2, 99)), "matched")
        self.assertEqual(m(P(1, 2)), "no")


unittest.main(globals())
