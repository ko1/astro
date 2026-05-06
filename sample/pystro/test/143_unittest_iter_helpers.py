import unittest
import itertools


class ZipMapEnumTest(unittest.TestCase):
    def test_zip(self):
        self.assertEqual(list(zip([1, 2, 3], "ab")), [(1, "a"), (2, "b")])
        self.assertEqual(list(zip([1, 2], "abc", [True, False, True])),
                         [(1, "a", True), (2, "b", False)])

    def test_zip_strict(self):
        with self.assertRaises(ValueError):
            list(zip([1, 2, 3], "ab", strict=True))

    def test_enum_start(self):
        self.assertEqual(list(enumerate("xyz", start=10)),
                         [(10, "x"), (11, "y"), (12, "z")])

    def test_map_multi(self):
        self.assertEqual(list(map(lambda a, b: a + b, [1, 2, 3], [10, 20, 30])),
                         [11, 22, 33])

    def test_map_uneven(self):
        self.assertEqual(list(map(lambda a, b: a + b, [1, 2, 3], [10, 20])),
                         [11, 22])

    def test_filter_none(self):
        self.assertEqual(list(filter(None, [0, 1, "", "x", [], [1]])),
                         [1, "x", [1]])


class SortMinMaxTest(unittest.TestCase):
    def test_stable_sort(self):
        d = [("a", 1), ("b", 2), ("a", 3), ("b", 4)]
        self.assertEqual(sorted(d, key=lambda x: x[0]),
                         [("a", 1), ("a", 3), ("b", 2), ("b", 4)])

    def test_max_default(self):
        self.assertEqual(max([], default="X"), "X")

    def test_sum_tuple_start(self):
        self.assertEqual(sum([(1, 2), (3, 4)], ()), (1, 2, 3, 4))


class ItertoolsTest(unittest.TestCase):
    def test_chain(self):
        self.assertEqual(list(itertools.chain([1, 2], [3, 4])), [1, 2, 3, 4])

    def test_chain_from_iter(self):
        self.assertEqual(list(itertools.chain.from_iterable([[1], [2, 3]])),
                         [1, 2, 3])

    def test_tee(self):
        a, b = itertools.tee([1, 2, 3])
        self.assertEqual(list(a), [1, 2, 3])
        self.assertEqual(list(b), [1, 2, 3])

    def test_cycle(self):
        it = iter(itertools.cycle([1, 2]))
        self.assertEqual([next(it) for _ in range(5)], [1, 2, 1, 2, 1])

    def test_repeat(self):
        self.assertEqual(list(itertools.repeat("x", 3)), ["x", "x", "x"])

    def test_product(self):
        self.assertEqual(list(itertools.product([1, 2], "ab")),
                         [(1, "a"), (1, "b"), (2, "a"), (2, "b")])
        self.assertEqual(list(itertools.product([0, 1], repeat=2)),
                         [(0, 0), (0, 1), (1, 0), (1, 1)])

    def test_perms_combs(self):
        self.assertEqual(list(itertools.permutations([1, 2, 3], 2)),
                         [(1, 2), (1, 3), (2, 1), (2, 3), (3, 1), (3, 2)])
        self.assertEqual(list(itertools.combinations([1, 2, 3], 2)),
                         [(1, 2), (1, 3), (2, 3)])
        self.assertEqual(list(itertools.combinations_with_replacement([1, 2], 2)),
                         [(1, 1), (1, 2), (2, 2)])

    def test_take_drop(self):
        self.assertEqual(list(itertools.dropwhile(lambda x: x < 3, [1, 2, 3, 4, 1])),
                         [3, 4, 1])
        self.assertEqual(list(itertools.takewhile(lambda x: x < 3, [1, 2, 3, 4, 1])),
                         [1, 2])

    def test_count_islice(self):
        it = itertools.count(start=10, step=2)
        self.assertEqual([next(it) for _ in range(4)], [10, 12, 14, 16])
        self.assertEqual(list(itertools.islice(range(10), 2, 8, 2)), [2, 4, 6])

    def test_filterfalse(self):
        self.assertEqual(list(itertools.filterfalse(lambda x: x < 3, [1, 2, 3, 4])),
                         [3, 4])

    def test_compress(self):
        self.assertEqual(list(itertools.compress("abcdef", [1, 0, 1, 0, 1, 0])),
                         ["a", "c", "e"])

    def test_pairwise(self):
        self.assertEqual(list(itertools.pairwise([1, 2, 3, 4])),
                         [(1, 2), (2, 3), (3, 4)])
        self.assertEqual(list(itertools.pairwise([1])), [])


unittest.main(globals())
