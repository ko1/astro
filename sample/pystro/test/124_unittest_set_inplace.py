import unittest


class SetInplaceTest(unittest.TestCase):
    def test_difference_update(self):
        s = {1, 2, 3, 4}
        s.difference_update({2, 3})
        self.assertEqual(s, {1, 4})

    def test_intersection_update(self):
        s = {1, 2, 3, 4}
        s.intersection_update({2, 3, 5})
        self.assertEqual(s, {2, 3})

    def test_symmetric_difference_update(self):
        s = {1, 2, 3}
        s.symmetric_difference_update({3, 4, 5})
        self.assertEqual(s, {1, 2, 4, 5})

    def test_chain(self):
        s = {1, 2, 3, 4, 5}
        s.update({6, 7})
        s.difference_update({1, 5})
        self.assertEqual(s, {2, 3, 4, 6, 7})


unittest.main(globals())
