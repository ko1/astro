import unittest


class GeneratorPlusYieldFromTest(unittest.TestCase):
    def test_yield_from_chain(self):
        def a():
            yield "a1"; yield "a2"
        def b():
            yield from a()
            yield "b1"
        self.assertEqual(list(b()), ["a1", "a2", "b1"])

    def test_yield_from_raise(self):
        def inner():
            yield 1
            raise ValueError("err")
        def outer():
            try:
                yield from inner()
            except ValueError as e:
                yield ("caught:" + str(e))
        self.assertEqual(list(outer()), [1, "caught:err"])


class DictExtraTest(unittest.TestCase):
    def test_dict_from_list(self):
        d = dict([("a", 1), ("b", 2)])
        self.assertEqual(d, {"a": 1, "b": 2})

    def test_dict_from_zip(self):
        d = dict(zip("xyz", [1, 2, 3]))
        self.assertEqual(d, {"x": 1, "y": 2, "z": 3})

    def test_fromkeys(self):
        self.assertEqual(dict.fromkeys(["a", "b"], 0), {"a": 0, "b": 0})
        self.assertEqual(dict.fromkeys(["a", "b"]), {"a": None, "b": None})

    def test_dict_equality_order(self):
        self.assertEqual({"a": 1, "b": 2}, {"b": 2, "a": 1})


class SliceAssignTest(unittest.TestCase):
    def test_simple(self):
        l = [0, 1, 2, 3, 4]
        l[1:3] = [99]
        self.assertEqual(l, [0, 99, 3, 4])

    def test_stepped(self):
        l = [0, 1, 2, 3, 4]
        l[::2] = [9, 9, 9]
        self.assertEqual(l, [9, 1, 9, 3, 9])

    def test_del_slice(self):
        l = [0, 1, 2, 3, 4]
        del l[1:3]
        self.assertEqual(l, [0, 3, 4])


class ListCmpTest(unittest.TestCase):
    def test_lex(self):
        self.assertLess([1, 2], [1, 3])
        self.assertLess([1, 2], [1, 2, 0])
        self.assertLess([], [1])


class TupleOpsTest(unittest.TestCase):
    def test_concat_repeat(self):
        self.assertEqual((1,) + (2,), (1, 2))
        self.assertEqual((1, 2) * 3, (1, 2, 1, 2, 1, 2))

    def test_index_count(self):
        t = (1, 2, 1, 3)
        self.assertEqual(t.index(2), 1)
        self.assertEqual(t.count(1), 2)


class FrozensetTest(unittest.TestCase):
    def test_hashable(self):
        fs = frozenset([1, 2, 3])
        d = {fs: "v"}
        self.assertEqual(d[frozenset([3, 2, 1])], "v")


unittest.main(globals())
