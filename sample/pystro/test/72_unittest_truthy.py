import unittest


class TruthyValueTest(unittest.TestCase):
    def test_complex_zero_is_false(self):
        self.assertFalse(bool(0j))
        self.assertFalse(bool(complex(0, 0)))

    def test_complex_nonzero_is_true(self):
        self.assertTrue(bool(complex(0, 1)))
        self.assertTrue(bool(complex(1, 0)))

    def test_bytes_empty_is_false(self):
        self.assertFalse(bool(b""))
        self.assertFalse(bool(bytearray()))

    def test_bytes_nonempty_is_true(self):
        self.assertTrue(bool(b"x"))
        self.assertTrue(bool(bytearray(b"x")))

    def test_set_empty_is_false(self):
        self.assertFalse(bool(set()))
        self.assertFalse(bool(frozenset()))

    def test_set_nonempty_is_true(self):
        self.assertTrue(bool({1}))
        self.assertTrue(bool(frozenset([1])))


class IfStatementBoolTest(unittest.TestCase):
    def test_if_complex_zero(self):
        if 0j:
            x = "true"
        else:
            x = "false"
        self.assertEqual(x, "false")

    def test_if_empty_bytes(self):
        if b"":
            x = "true"
        else:
            x = "false"
        self.assertEqual(x, "false")


class ChainCompareTest(unittest.TestCase):
    def test_basic(self):
        self.assertTrue(0 < 5 < 10)
        self.assertFalse(0 < 5 < 3)

    def test_mixed(self):
        self.assertTrue(0 < 5 > 3)
        self.assertTrue("a" < "b" == "b")


class JsonRoundtripTest(unittest.TestCase):
    def test_complex(self):
        import json
        data = {
            "name": "Alice", "age": 30,
            "list": [1, 2, 3],
            "nested": {"a": 1, "b": True},
            "nullval": None,
            "bool": False,
            "float": 3.14,
        }
        s = json.dumps(data)
        self.assertEqual(json.loads(s), data)


class StringPctFormatTest(unittest.TestCase):
    def test_format(self):
        self.assertEqual("%d %s" % (42, "hi"), "42 hi")
        self.assertEqual("%5.2f" % 3.14159, " 3.14")
        self.assertEqual("%05d" % 42, "00042")
        self.assertEqual("%x" % 255, "ff")


class WalrusTest(unittest.TestCase):
    def test_in_if(self):
        if (n := 5) > 3:
            self.assertEqual(n, 5)
        else:
            self.fail()

    def test_in_listcomp(self):
        data = [1, 2, 3, 4, 5]
        result = [y for x in data if (y := x * 2) > 4]
        self.assertEqual(result, [6, 8, 10])

    def test_in_while(self):
        items = iter([0, 0, 5, 0])
        while (n := next(items)) == 0:
            pass
        self.assertEqual(n, 5)


class GeneratorAdvancedTest(unittest.TestCase):
    def test_yield_from_basic(self):
        def inner():
            yield 1
            yield 2
        def outer():
            yield 0
            yield from inner()
            yield 3
        self.assertEqual(list(outer()), [0, 1, 2, 3])

    def test_close_runs_finally(self):
        log = []
        def gen():
            try:
                yield 1
                yield 2
            finally:
                log.append("cleanup")
        g = gen()
        next(g)
        g.close()
        self.assertEqual(log, ["cleanup"])


unittest.main(globals())
