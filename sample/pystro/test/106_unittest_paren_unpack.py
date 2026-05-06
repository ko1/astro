import unittest


class ParenUnpackTest(unittest.TestCase):
    def test_paren(self):
        (a, b) = (1, 2)
        self.assertEqual(a, 1)
        self.assertEqual(b, 2)

    def test_bracket(self):
        [a, b] = [3, 4]
        self.assertEqual(a, 3)
        self.assertEqual(b, 4)

    def test_paren_star(self):
        (a, *rest) = (1, 2, 3, 4)
        self.assertEqual(a, 1)
        self.assertEqual(rest, [2, 3, 4])

    def test_three(self):
        [a, b, c] = [10, 20, 30]
        self.assertEqual((a, b, c), (10, 20, 30))


unittest.main(globals())
