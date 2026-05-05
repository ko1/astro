import unittest


class CountDictTest(unittest.TestCase):
    def test_super_setitem(self):
        class CD(dict):
            def __init__(self):
                super().__init__()
                self.count = 0
            def __setitem__(self, key, value):
                self.count += 1
                super().__setitem__(key, value)
        d = CD()
        d["a"] = 1
        d["b"] = 2
        d["a"] = 3
        self.assertEqual(d.count, 3)
        self.assertEqual(d["a"], 3)
        self.assertEqual(len(d), 2)


class TypedListTest(unittest.TestCase):
    def test_typed_append(self):
        class TL(list):
            def __init__(self, t):
                super().__init__()
                self.t = t
            def append(self, x):
                if not isinstance(x, self.t):
                    raise TypeError(f"expected {self.t.__name__}")
                super().append(x)
        tl = TL(int)
        tl.append(1)
        tl.append(2)
        with self.assertRaises(TypeError):
            tl.append("a")
        self.assertEqual(len(tl), 2)


class OrderedDictTest(unittest.TestCase):
    def test_iter(self):
        class OD(dict):
            def __init__(self):
                super().__init__()
                self._keys = []
            def __setitem__(self, k, v):
                if k not in self:
                    self._keys.append(k)
                super().__setitem__(k, v)
            def __iter__(self):
                return iter(self._keys)
        od = OD()
        od["c"] = 3
        od["a"] = 1
        od["b"] = 2
        self.assertEqual(list(od), ["c", "a", "b"])


class UserIterReturnsBuiltinTest(unittest.TestCase):
    def test_returns_list_iter(self):
        class O:
            def __iter__(self):
                return iter([1, 2, 3])
        self.assertEqual(list(O()), [1, 2, 3])

    def test_returns_dict_iter(self):
        class M:
            def __iter__(self):
                return iter({"a": 1, "b": 2})
        # Order isn't guaranteed by dict iter in CPython 3.7+ — but for
        # small dicts it'll be insertion order.
        self.assertEqual(sorted(list(M())), ["a", "b"])


unittest.main(globals())
