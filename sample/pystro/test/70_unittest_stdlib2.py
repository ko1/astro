import unittest


class CollectionsTest(unittest.TestCase):
    def test_namedtuple_attrs(self):
        from collections import namedtuple
        Point = namedtuple("Point", ["x", "y"])
        p = Point(3, 4)
        self.assertEqual(p.x, 3)
        self.assertEqual(p.y, 4)
        self.assertEqual(p[0], 3)
        self.assertEqual(p[1], 4)
        self.assertEqual(list(p), [3, 4])
        self.assertEqual(len(p), 2)

    def test_namedtuple_kwargs(self):
        from collections import namedtuple
        Point = namedtuple("Point", "x y")
        p = Point(x=1, y=2)
        self.assertEqual(p.x, 1)
        self.assertEqual(p.y, 2)

    def test_namedtuple_asdict(self):
        from collections import namedtuple
        P = namedtuple("P", "a b")
        p = P(10, 20)
        self.assertEqual(p._asdict(), {"a": 10, "b": 20})

    def test_chainmap(self):
        from collections import ChainMap
        c = ChainMap({"a": 1}, {"b": 2, "a": 99})
        self.assertEqual(c["a"], 1)    # first wins
        self.assertEqual(c["b"], 2)
        self.assertIn("a", c)
        self.assertIn("b", c)
        self.assertEqual(sorted(list(c)), ["a", "b"])

    def test_dict_from_defaultdict(self):
        from collections import defaultdict
        d = defaultdict(list)
        d["a"].append(1)
        d["a"].append(2)
        d["b"].append(3)
        self.assertEqual(dict(d), {"a": [1, 2], "b": [3]})


class ItertoolsTest(unittest.TestCase):
    def test_chain_from_iterable(self):
        import itertools
        self.assertEqual(list(itertools.chain.from_iterable([[1, 2], [3, 4]])),
                         [1, 2, 3, 4])

    def test_compress(self):
        import itertools
        self.assertEqual(list(itertools.compress("ABCDEF", [1, 0, 1, 0, 1, 1])),
                         ["A", "C", "E", "F"])

    def test_filterfalse(self):
        import itertools
        self.assertEqual(list(itertools.filterfalse(lambda x: x % 2, [1, 2, 3, 4])),
                         [2, 4])

    def test_starmap(self):
        import itertools
        self.assertEqual(list(itertools.starmap(lambda a, b: a + b, [(1, 2), (3, 4)])),
                         [3, 7])

    def test_zip_longest(self):
        import itertools
        self.assertEqual(list(itertools.zip_longest([1, 2, 3], "ab", fillvalue="?")),
                         [(1, "a"), (2, "b"), (3, "?")])

    def test_combinations_with_replacement(self):
        import itertools
        self.assertEqual(list(itertools.combinations_with_replacement([1, 2], 3)),
                         [(1, 1, 1), (1, 1, 2), (1, 2, 2), (2, 2, 2)])

    def test_pairwise(self):
        import itertools
        self.assertEqual(list(itertools.pairwise([1, 2, 3, 4])),
                         [(1, 2), (2, 3), (3, 4)])

    def test_groupby(self):
        import itertools
        result = [(k, list(g)) for k, g in itertools.groupby("AAABBCCAA")]
        self.assertEqual(result, [("A", ["A", "A", "A"]), ("B", ["B", "B"]),
                                  ("C", ["C", "C"]), ("A", ["A", "A"])])


class OperatorIopsTest(unittest.TestCase):
    def test_iadd_list(self):
        import operator
        a = [1, 2]
        result = operator.iadd(a, [3, 4])
        self.assertEqual(result, [1, 2, 3, 4])

    def test_index(self):
        import operator
        self.assertEqual(operator.index(5), 5)

    def test_length_hint(self):
        import operator
        self.assertEqual(operator.length_hint([1, 2, 3]), 3)


class HeapqMoreTest(unittest.TestCase):
    def test_nlargest(self):
        import heapq
        self.assertEqual(heapq.nlargest(3, [3, 1, 4, 1, 5, 9, 2, 6]), [9, 6, 5])

    def test_nsmallest(self):
        import heapq
        self.assertEqual(heapq.nsmallest(3, [3, 1, 4, 1, 5, 9, 2, 6]), [1, 1, 2])

    def test_heapify(self):
        import heapq
        a = [3, 1, 4, 1, 5]
        heapq.heapify(a)
        self.assertEqual(a[0], 1)


class TempfileTest(unittest.TestCase):
    def test_named_tempfile(self):
        import tempfile
        import os
        with tempfile.NamedTemporaryFile(mode="w") as f:
            f.write("hello")
            name = f.name
        # File should be deleted.
        self.assertFalse(os.path.exists(name))


unittest.main(globals())
