import unittest


class DictProtocolTest(unittest.TestCase):
    def test_custom_hash_eq(self):
        class K:
            def __init__(self, n): self.n = n
            def __eq__(self, o): return isinstance(o, K) and self.n == o.n
            def __hash__(self): return self.n
        d = {}
        d[K(1)] = "a"
        d[K(2)] = "b"
        self.assertEqual(d[K(1)], "a")
        self.assertEqual(d[K(2)], "b")

    def test_bool_int_collision(self):
        # CPython: hash(True) == hash(1), True == 1, so they collide.
        d = {1: "int_one", True: "bool_true"}
        self.assertEqual(len(d), 1)
        self.assertEqual(d[1], "bool_true")
        self.assertEqual(d[True], "bool_true")

    def test_unhashable_list(self):
        with self.assertRaises(TypeError):
            d = {[1, 2]: "x"}

    def test_unhashable_dict(self):
        with self.assertRaises(TypeError):
            d = {{}: "x"}

    def test_unhashable_set(self):
        with self.assertRaises(TypeError):
            d = {set(): "x"}

    def test_keyerror_repr(self):
        d = {"a": 1}
        try:
            del d["missing"]
        except KeyError as e:
            self.assertIn("missing", str(e))

    def test_frozenset_key(self):
        d = {frozenset([1, 2]): "x"}
        self.assertEqual(d[frozenset([1, 2])], "x")
        self.assertEqual(d[frozenset([2, 1])], "x")

    def test_setdefault_no_override(self):
        d = {"a": 1}
        d.setdefault("a", 99)
        self.assertEqual(d["a"], 1)

    def test_dict_union(self):
        a = {"a": 1, "b": 2}
        b = {"b": 3, "c": 4}
        c = a | b
        self.assertEqual(c, {"a": 1, "b": 3, "c": 4})


unittest.main(globals())
