# Adapted from CPython test_dict.py.

import unittest


class DictTest(unittest.TestCase):
    def test_construct(self):
        self.assertEqual(dict(), {})
        self.assertEqual({}, {})
        self.assertEqual({"a": 1}, {"a": 1})

    def test_get_set(self):
        d = {"a": 1}
        d["b"] = 2
        self.assertEqual(d["a"], 1)
        self.assertEqual(d["b"], 2)
        d["a"] = 10
        self.assertEqual(d["a"], 10)

    def test_membership(self):
        d = {"a": 1, "b": 2}
        self.assertIn("a", d)
        self.assertNotIn("c", d)

    def test_get_default(self):
        d = {"a": 1}
        self.assertEqual(d.get("a"), 1)
        self.assertEqual(d.get("b"), None)
        self.assertEqual(d.get("b", 99), 99)

    def test_keys_values_items(self):
        d = {"a": 1, "b": 2}
        self.assertEqual(list(d.keys()), ["a", "b"])
        self.assertEqual(list(d.values()), [1, 2])
        self.assertEqual(list(d.items()), [("a", 1), ("b", 2)])

    def test_pop(self):
        d = {"a": 1, "b": 2}
        self.assertEqual(d.pop("a"), 1)
        self.assertEqual(d, {"b": 2})
        self.assertEqual(d.pop("z", 99), 99)

    def test_popitem(self):
        d = {"a": 1, "b": 2, "c": 3}
        k, v = d.popitem()
        self.assertEqual((k, v), ("c", 3))

    def test_update(self):
        d = {"a": 1}
        d.update({"b": 2, "a": 10})
        self.assertEqual(d, {"a": 10, "b": 2})

    def test_setdefault(self):
        d = {"a": 1}
        self.assertEqual(d.setdefault("a", 99), 1)
        self.assertEqual(d.setdefault("b", 99), 99)
        self.assertEqual(d, {"a": 1, "b": 99})

    def test_clear(self):
        d = {"a": 1, "b": 2}
        d.clear()
        self.assertEqual(d, {})

    def test_copy(self):
        d = {"a": 1}
        d2 = d.copy()
        d2["b"] = 2
        self.assertEqual(d, {"a": 1})
        self.assertEqual(d2, {"a": 1, "b": 2})

    def test_iter(self):
        d = {"a": 1, "b": 2}
        keys = []
        for k in d:
            keys.append(k)
        self.assertEqual(keys, ["a", "b"])

    def test_len(self):
        self.assertEqual(len({}), 0)
        self.assertEqual(len({"a": 1, "b": 2}), 2)

    def test_insertion_order(self):
        d = {}
        for c in "zebra":
            d[c] = ord(c)
        self.assertEqual(list(d.keys()), ["z", "e", "b", "r", "a"])

    def test_comp(self):
        d = {x: x*x for x in range(5)}
        self.assertEqual(d, {0: 0, 1: 1, 2: 4, 3: 9, 4: 16})

    def test_keyerror(self):
        d = {}
        try:
            v = d["x"]
            self.fail("expected KeyError")
        except KeyError:
            pass

    def test_del(self):
        d = {"a": 1, "b": 2}
        del d["a"]
        self.assertEqual(d, {"b": 2})

    def test_type(self):
        self.assertIs(type({}), dict)
        self.assertIsInstance({}, dict)


unittest.main(globals())
