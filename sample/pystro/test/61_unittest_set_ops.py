import unittest


class SetOpTest(unittest.TestCase):
    def test_union(self):
        a = {1, 2, 3}
        b = {3, 4, 5}
        self.assertEqual(a | b, {1, 2, 3, 4, 5})
        self.assertEqual(a.union(b), {1, 2, 3, 4, 5})

    def test_intersection(self):
        a = {1, 2, 3, 4}
        b = {3, 4, 5}
        self.assertEqual(a & b, {3, 4})
        self.assertEqual(a.intersection(b), {3, 4})

    def test_difference(self):
        a = {1, 2, 3}
        b = {2, 3}
        self.assertEqual(a - b, {1})
        self.assertEqual(a.difference(b), {1})

    def test_symmetric_difference(self):
        a = {1, 2, 3}
        b = {3, 4, 5}
        self.assertEqual(a ^ b, {1, 2, 4, 5})
        self.assertEqual(a.symmetric_difference(b), {1, 2, 4, 5})

    def test_subset_super_disjoint(self):
        a = {1, 2}
        b = {1, 2, 3}
        self.assertTrue(a.issubset(b))
        self.assertTrue(b.issuperset(a))
        self.assertTrue({4, 5}.isdisjoint({1, 2, 3}))
        self.assertFalse({1, 5}.isdisjoint({1, 2, 3}))

    def test_clear_copy_update(self):
        a = {1, 2, 3}
        b = a.copy()
        a.clear()
        self.assertEqual(a, set())
        self.assertEqual(b, {1, 2, 3})
        b.update({4, 5})
        self.assertEqual(b, {1, 2, 3, 4, 5})


class CustomContainerTest(unittest.TestCase):
    def test_contains_iter(self):
        class R:
            def __init__(self, data):
                self.data = data
                self.idx = len(data)
            def __iter__(self):
                self.idx = len(self.data)
                return self
            def __next__(self):
                if self.idx == 0:
                    raise StopIteration
                self.idx -= 1
                return self.data[self.idx]
        r = R([10, 20, 30])
        self.assertTrue(20 in r)
        self.assertFalse(99 in r)

    def test_delitem(self):
        class V:
            def __init__(self):
                self.d = {}
            def __getitem__(self, k):
                return self.d[k]
            def __setitem__(self, k, v):
                self.d[k] = v
            def __delitem__(self, k):
                del self.d[k]
        v = V()
        v["a"] = 1
        v["b"] = 2
        del v["a"]
        self.assertEqual(v.d, {"b": 2})


unittest.main(globals())
