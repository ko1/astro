# Adapted from CPython test_list.py.

import unittest


class ListTest(unittest.TestCase):
    def test_construct(self):
        self.assertEqual(list(), [])
        self.assertEqual(list(range(5)), [0, 1, 2, 3, 4])
        self.assertEqual(list("abc"), ["a", "b", "c"])

    def test_index(self):
        a = [10, 20, 30]
        self.assertEqual(a[0], 10)
        self.assertEqual(a[-1], 30)
        self.assertEqual(a[1], 20)

    def test_slice(self):
        a = [1, 2, 3, 4, 5]
        self.assertEqual(a[:3], [1, 2, 3])
        self.assertEqual(a[2:], [3, 4, 5])
        self.assertEqual(a[1:4], [2, 3, 4])
        self.assertEqual(a[::2], [1, 3, 5])
        self.assertEqual(a[::-1], [5, 4, 3, 2, 1])

    def test_mut(self):
        a = [1, 2, 3]
        a[0] = 100
        self.assertEqual(a, [100, 2, 3])
        a[-1] = 999
        self.assertEqual(a, [100, 2, 999])

    def test_slice_assign(self):
        a = [1, 2, 3, 4, 5]
        a[1:3] = [20, 30, 40]
        self.assertEqual(a, [1, 20, 30, 40, 4, 5])

    def test_append_pop(self):
        a = []
        a.append(1)
        a.append(2)
        self.assertEqual(a, [1, 2])
        self.assertEqual(a.pop(), 2)
        self.assertEqual(a, [1])

    def test_insert(self):
        a = [1, 3]
        a.insert(1, 2)
        self.assertEqual(a, [1, 2, 3])
        a.insert(0, 0)
        self.assertEqual(a, [0, 1, 2, 3])

    def test_extend(self):
        a = [1, 2]
        a.extend([3, 4])
        self.assertEqual(a, [1, 2, 3, 4])

    def test_remove_count(self):
        a = [1, 2, 1, 3, 1]
        self.assertEqual(a.count(1), 3)
        a.remove(1)
        self.assertEqual(a, [2, 1, 3, 1])

    def test_index_method(self):
        a = [10, 20, 30, 20]
        self.assertEqual(a.index(20), 1)

    def test_reverse(self):
        a = [1, 2, 3]
        a.reverse()
        self.assertEqual(a, [3, 2, 1])

    def test_sort(self):
        a = [3, 1, 4, 1, 5, 9, 2, 6]
        a.sort()
        self.assertEqual(a, [1, 1, 2, 3, 4, 5, 6, 9])
        a.sort(reverse=True)
        self.assertEqual(a, [9, 6, 5, 4, 3, 2, 1, 1])
        a = [(1, "b"), (2, "a"), (3, "c")]
        a.sort(key=lambda x: x[1])
        self.assertEqual(a, [(2, "a"), (1, "b"), (3, "c")])

    def test_concat(self):
        self.assertEqual([1, 2] + [3, 4], [1, 2, 3, 4])
        self.assertEqual([0] * 3, [0, 0, 0])
        self.assertEqual(3 * [0], [0, 0, 0])

    def test_membership(self):
        self.assertIn(3, [1, 2, 3])
        self.assertNotIn(99, [1, 2, 3])

    def test_len(self):
        self.assertEqual(len([]), 0)
        self.assertEqual(len([1]), 1)
        self.assertEqual(len(list(range(100))), 100)

    def test_iter(self):
        out = []
        for x in [1, 2, 3]:
            out.append(x)
        self.assertEqual(out, [1, 2, 3])

    def test_comp(self):
        self.assertEqual([x*x for x in range(5)], [0, 1, 4, 9, 16])
        self.assertEqual([x for x in range(10) if x % 2 == 0], [0, 2, 4, 6, 8])

    def test_nested(self):
        m = [[1, 2], [3, 4]]
        self.assertEqual(m[0], [1, 2])
        self.assertEqual(m[1][1], 4)

    def test_copy(self):
        a = [1, 2, 3]
        b = a[:]
        b.append(99)
        self.assertEqual(a, [1, 2, 3])
        self.assertEqual(b, [1, 2, 3, 99])

    def test_clear(self):
        a = [1, 2, 3]
        a.clear()
        self.assertEqual(a, [])

    def test_type(self):
        self.assertIs(type([]), list)
        self.assertIsInstance([1, 2], list)


unittest.main(globals())
