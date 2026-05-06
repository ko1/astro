import unittest


class ListIaddTest(unittest.TestCase):
    def test_list_plus_str(self):
        l = [1, 2]
        l += "ab"
        self.assertEqual(l, [1, 2, "a", "b"])

    def test_list_plus_range(self):
        l = [1, 2]
        l += range(3)
        self.assertEqual(l, [1, 2, 0, 1, 2])

    def test_list_plus_dict_keys(self):
        l = []
        l += {"a": 1, "b": 2}.keys()
        self.assertEqual(sorted(l), ["a", "b"])

    def test_list_plus_set(self):
        l = []
        l += {1, 2, 3}
        self.assertEqual(sorted(l), [1, 2, 3])

    def test_list_plus_gen(self):
        l = [0]
        l += (x*x for x in range(3))
        self.assertEqual(l, [0, 0, 1, 4])


unittest.main(globals())
