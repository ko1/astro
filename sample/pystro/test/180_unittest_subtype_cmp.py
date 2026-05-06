import unittest


class IntSubtypeCmpTest(unittest.TestCase):
    def test_max_min(self):
        class MyInt(int): pass
        l = [MyInt(3), MyInt(1), MyInt(2)]
        self.assertEqual(max(l), 3)
        self.assertEqual(min(l), 1)

    def test_sort(self):
        class MyInt(int): pass
        l = [MyInt(3), MyInt(1), MyInt(2)]
        self.assertEqual(sorted(l), [1, 2, 3])


class FloatSubtypeTest(unittest.TestCase):
    def test_arith(self):
        class MyFloat(float): pass
        self.assertEqual(MyFloat(3.14) * 2, 6.28)
        self.assertTrue(isinstance(MyFloat(1.0), float))


class StrSubtypeTest(unittest.TestCase):
    def test_methods(self):
        class MyStr(str): pass
        s = MyStr("hello")
        self.assertEqual(s.upper(), "HELLO")
        self.assertEqual(s[1:4], "ell")
        self.assertEqual(s + " world", "hello world")


class ListSubtypeTest(unittest.TestCase):
    def test_basic(self):
        class MyList(list): pass
        ml = MyList([1, 2, 3])
        ml.append(4)
        self.assertEqual(list(ml), [1, 2, 3, 4])
        self.assertEqual(len(ml), 4)


class TupleSubtypeTest(unittest.TestCase):
    def test_basic(self):
        class MyTuple(tuple): pass
        mt = MyTuple([1, 2, 3])
        self.assertEqual(mt[1], 2)
        self.assertEqual(len(mt), 3)


unittest.main(globals())
