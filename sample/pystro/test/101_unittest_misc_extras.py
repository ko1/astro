import unittest


class EllipsisTest(unittest.TestCase):
    def test_literal(self):
        self.assertIs(..., Ellipsis)

    def test_in_function_body(self):
        def f():
            ...
        self.assertIsNone(f())

    def test_type(self):
        self.assertEqual(type(...).__name__, "ellipsis")


class HashNoneTest(unittest.TestCase):
    def test_unhashable_via_None(self):
        class U:
            __hash__ = None
        with self.assertRaises(TypeError):
            hash(U())
        with self.assertRaises(TypeError):
            {U(): 1}


class WalrusTest(unittest.TestCase):
    def test_in_if(self):
        if (n := 5) > 0:
            self.assertEqual(n, 5)

    def test_in_while(self):
        out = []
        i = 0
        data = [1, 2, 3]
        while (x := data[i] if i < len(data) else None) is not None:
            out.append(x)
            i += 1
        self.assertEqual(out, [1, 2, 3])

    def test_in_comp(self):
        result = [y for x in [1, 2, 3, 4] if (y := x * 2) > 4]
        self.assertEqual(result, [6, 8])


unittest.main(globals())
