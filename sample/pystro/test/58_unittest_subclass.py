# Built-in type subclassing.

import unittest


class ListSubclassTest(unittest.TestCase):
    def test_basic(self):
        class MyList(list):
            pass
        m = MyList([1, 2, 3])
        m.append(4)
        self.assertEqual(len(m), 4)
        self.assertEqual(m[0], 1)
        self.assertEqual(m[3], 4)

    def test_iter(self):
        class MyList(list):
            pass
        m = MyList([10, 20, 30])
        out = []
        for x in m:
            out.append(x)
        self.assertEqual(out, [10, 20, 30])

    def test_setitem(self):
        class MyList(list):
            pass
        m = MyList([1, 2, 3])
        m[0] = 99
        self.assertEqual(m[0], 99)

    def test_methods_inherited(self):
        class MyList(list):
            pass
        m = MyList([3, 1, 2])
        m.sort()
        self.assertEqual(list(m), [1, 2, 3])

    def test_user_method(self):
        class CustomList(list):
            def double(self):
                return [x * 2 for x in self]
        m = CustomList([1, 2, 3])
        self.assertEqual(m.double(), [2, 4, 6])

    def test_isinstance(self):
        class M(list): pass
        m = M([1, 2])
        self.assertTrue(isinstance(m, M))
        self.assertTrue(issubclass(M, list))


class DictSubclassTest(unittest.TestCase):
    def test_basic(self):
        class MyDict(dict):
            pass
        d = MyDict()
        d["a"] = 1
        d["b"] = 2
        self.assertEqual(d["a"], 1)
        self.assertEqual(len(d), 2)

    def test_iter(self):
        class MyDict(dict):
            pass
        d = MyDict()
        d["x"] = 1
        d["y"] = 2
        keys = []
        for k in d:
            keys.append(k)
        self.assertEqual(keys, ["x", "y"])


class StrSubclassTest(unittest.TestCase):
    def test_basic(self):
        class MyStr(str):
            pass
        s = MyStr("hello")
        self.assertEqual(len(s), 5)
        self.assertEqual(s.upper(), "HELLO")

    def test_user_method(self):
        class Tagged(str):
            def shout(self):
                return self.upper() + "!"
        s = Tagged("hi")
        self.assertEqual(s.shout(), "HI!")


unittest.main(globals())
