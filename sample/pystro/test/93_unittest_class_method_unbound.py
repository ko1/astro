import unittest


class UnboundBuiltinMethodTest(unittest.TestCase):
    def test_str_lower_as_key(self):
        self.assertEqual(sorted(["B", "a"], key=str.lower), ["a", "B"])

    def test_str_upper_call(self):
        self.assertEqual(str.upper("hello"), "HELLO")

    def test_list_append(self):
        L = []
        list.append(L, 1)
        list.append(L, 2)
        self.assertEqual(L, [1, 2])

    def test_dict_get(self):
        d = {"a": 1}
        self.assertEqual(dict.get(d, "a"), 1)
        self.assertEqual(dict.get(d, "b", 99), 99)


class FormatNestedTest(unittest.TestCase):
    def test_attr_in_format(self):
        class P:
            def __init__(self, x, y): self.x, self.y = x, y
        p = P(1, 2)
        self.assertEqual("{p.x} {p.y}".format(p=p), "1 2")

    def test_index_in_format(self):
        self.assertEqual("{x[0]}-{x[1]}".format(x=[10, 20]), "10-20")


class NestedForTargetTest(unittest.TestCase):
    def test_nested(self):
        out = []
        for i, (a, b) in enumerate([(1, 2), (3, 4)]):
            out.append((i, a, b))
        self.assertEqual(out, [(0, 1, 2), (1, 3, 4)])


class MROIncludesObjectTest(unittest.TestCase):
    def test_basic(self):
        class A: pass
        self.assertEqual(A.__mro__, (A, object))

    def test_inherited(self):
        class B: pass
        class C(B): pass
        self.assertEqual(C.__mro__, (C, B, object))


unittest.main(globals())
