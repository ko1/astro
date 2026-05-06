import unittest
from collections import ChainMap, namedtuple, deque, Counter, OrderedDict


class ChainMapMutateTest(unittest.TestCase):
    def test_setitem(self):
        cm = ChainMap({"a": 1}, {"b": 2})
        cm["c"] = 3
        self.assertEqual(cm.maps[0], {"a": 1, "c": 3})

    def test_pop(self):
        cm = ChainMap({"a": 1, "b": 2})
        self.assertEqual(cm.pop("a"), 1)
        self.assertNotIn("a", cm)

    def test_pop_missing_default(self):
        cm = ChainMap({})
        self.assertEqual(cm.pop("z", "default"), "default")

    def test_new_child_writable(self):
        cm = ChainMap({"a": 1})
        c = cm.new_child()
        c["b"] = 2
        self.assertIn("b", c)
        self.assertNotIn("b", cm)


class NamedTupleMakeTest(unittest.TestCase):
    def test_make(self):
        NT = namedtuple("Pt", "x y")
        p = NT._make([1, 2])
        self.assertEqual((p.x, p.y), (1, 2))

    def test_asdict_make_roundtrip(self):
        NT = namedtuple("R", "a b c")
        r = NT(10, 20, 30)
        d = r._asdict()
        # _make from values in field order.
        r2 = NT._make([d["a"], d["b"], d["c"]])
        self.assertEqual(r, r2)


class DequeRotateTest(unittest.TestCase):
    def test_rotate(self):
        d = deque([1, 2, 3, 4, 5])
        d.rotate(2)
        self.assertEqual(list(d), [4, 5, 1, 2, 3])
        d.rotate(-2)
        self.assertEqual(list(d), [1, 2, 3, 4, 5])


class CounterMathTest(unittest.TestCase):
    def test_add(self):
        c = Counter("aab") + Counter("ab")
        self.assertEqual(c, Counter({"a": 3, "b": 2}))

    def test_sub(self):
        c = Counter("aaab") - Counter("ab")
        self.assertEqual(c, Counter({"a": 2}))

    def test_intersection(self):
        c = Counter("aaab") & Counter("ab")
        self.assertEqual(c, Counter({"a": 1, "b": 1}))


unittest.main(globals())
