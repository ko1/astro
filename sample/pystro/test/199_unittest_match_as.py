import unittest


class MatchAsTest(unittest.TestCase):
    def test_sequence_as(self):
        def f(v):
            match v:
                case [1, 2, *rest] as L:
                    return ("ok", L, rest)
                case _:
                    return "no"
        self.assertEqual(f([1, 2, 3, 4]), ("ok", [1, 2, 3, 4], [3, 4]))

    def test_with_guard(self):
        def f(v):
            match v:
                case [a, b] as pair if a < b:
                    return ("ordered", pair)
                case _:
                    return "other"
        self.assertEqual(f([1, 2]), ("ordered", [1, 2]))
        self.assertEqual(f([3, 1]), "other")

    def test_class_as(self):
        class P:
            __match_args__ = ("x",)
            def __init__(self, x): self.x = x
        def f(v):
            match v:
                case P(0) as p:
                    return ("zero", p)
                case _:
                    return "no"
        result = f(P(0))
        self.assertEqual(result[0], "zero")

    def test_or_as(self):
        def f(v):
            match v:
                case 1 | 2 as x:
                    return ("ok", x)
                case _:
                    return "no"
        self.assertEqual(f(1), ("ok", 1))
        self.assertEqual(f(2), ("ok", 2))
        self.assertEqual(f(3), "no")


unittest.main(globals())
