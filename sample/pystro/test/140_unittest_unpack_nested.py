import unittest


class NestedUnpackTest(unittest.TestCase):
    def test_simple_nested(self):
        (a, b), c = ((1, 2), 3)
        self.assertEqual((a, b, c), (1, 2, 3))

    def test_deeper(self):
        ((a, b), (c, d)) = ((1, 2), (3, 4))
        self.assertEqual((a, b, c, d), (1, 2, 3, 4))

    def test_brack_form(self):
        [(a, b), c] = [(10, 20), 30]
        self.assertEqual((a, b, c), (10, 20, 30))

    def test_for_paren_target(self):
        results = []
        for (a, b) in [(1, 2), (3, 4)]:
            results.append((a, b))
        self.assertEqual(results, [(1, 2), (3, 4)])

    def test_for_nested_paren(self):
        results = []
        for (a, b), c in [((1, 2), 3), ((4, 5), 6)]:
            results.append((a, b, c))
        self.assertEqual(results, [(1, 2, 3), (4, 5, 6)])


unittest.main(globals())
