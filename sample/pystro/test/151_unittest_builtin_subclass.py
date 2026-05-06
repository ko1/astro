import unittest


class IntSubclassTest(unittest.TestCase):
    def test_int_repr(self):
        class MyInt(int): pass
        self.assertEqual(str(MyInt(42)), "42")
        self.assertEqual(repr(MyInt(42)), "42")

    def test_int_arith(self):
        class MyInt(int): pass
        self.assertEqual(MyInt(2) + 3, 5)
        self.assertEqual(MyInt(10) - MyInt(3), 7)

    def test_int_isinstance(self):
        class MyInt(int): pass
        self.assertIsInstance(MyInt(0), int)


class ListSubclassTest(unittest.TestCase):
    def test_list_repr(self):
        class MyList(list): pass
        self.assertEqual(str(MyList([1, 2, 3])), "[1, 2, 3]")

    def test_list_iter(self):
        class MyList(list): pass
        self.assertEqual(sum(MyList([1, 2, 3])), 6)


class StrSubclassTest(unittest.TestCase):
    def test_str_repr(self):
        class MyStr(str): pass
        self.assertEqual(str(MyStr("hi")), "hi")

    def test_str_concat(self):
        class MyStr(str): pass
        s = MyStr("a") + "b"
        self.assertEqual(s, "ab")


class DictSubclassTest(unittest.TestCase):
    def test_dict_repr(self):
        class MyDict(dict): pass
        d = MyDict({"a": 1})
        self.assertEqual(str(d), "{'a': 1}")

    def test_dict_lookup(self):
        class MyDict(dict): pass
        d = MyDict({"a": 1, "b": 2})
        self.assertEqual(d["a"], 1)


unittest.main(globals())
