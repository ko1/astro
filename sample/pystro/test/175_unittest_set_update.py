import unittest


class SetUpdateTest(unittest.TestCase):
    def test_single(self):
        s = {1}
        s.update([2, 3])
        self.assertEqual(s, {1, 2, 3})

    def test_multiple(self):
        s = {1}
        s.update([2, 3], [4, 5])
        self.assertEqual(s, {1, 2, 3, 4, 5})

    def test_three_iterables(self):
        s = set()
        s.update([1, 2], "ab", {3, 4})
        self.assertEqual(s, {1, 2, "a", "b", 3, 4})

    def test_with_dict(self):
        s = set()
        s.update({"a": 1, "b": 2})
        self.assertEqual(s, {"a", "b"})


unittest.main(globals())
