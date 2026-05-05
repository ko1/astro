import unittest


class GetattrFallbackTest(unittest.TestCase):
    def test_getattr_dunder(self):
        class C:
            def __getattr__(self, name):
                return "missing:" + name
        c = C()
        self.assertEqual(c.x, "missing:x")
        self.assertEqual(c.foo, "missing:foo")

    def test_existing_attr_no_fallback(self):
        class C:
            def __getattr__(self, name):
                return "fallback"
        c = C()
        c.real = 42
        self.assertEqual(c.real, 42)


class StrMethodTest(unittest.TestCase):
    def test_isidentifier(self):
        self.assertTrue("_abc".isidentifier())
        self.assertTrue("a1".isidentifier())
        self.assertFalse("1a".isidentifier())
        self.assertFalse("".isidentifier())
        self.assertFalse("a-b".isidentifier())

    def test_isprintable(self):
        self.assertTrue("abc".isprintable())
        self.assertTrue("".isprintable())

    def test_istitle(self):
        self.assertTrue("Hello World".istitle())
        self.assertFalse("Hello world".istitle())
        self.assertFalse("".istitle())

    def test_replace_count(self):
        self.assertEqual("aaaa".replace("a", "b", 2), "bbaa")
        self.assertEqual("aaaa".replace("a", "b", 0), "aaaa")

    def test_join_iterable(self):
        self.assertEqual(",".join(map(str, [1, 2, 3])), "1,2,3")
        self.assertEqual("-".join(str(x) for x in range(3)), "0-1-2")

    def test_find_with_range(self):
        self.assertEqual("abcabc".find("a"), 0)
        self.assertEqual("abcabc".find("a", 1), 3)
        self.assertEqual("abcabc".find("a", 4), -1)


class IsinstanceTest(unittest.TestCase):
    def test_object_universal(self):
        self.assertTrue(isinstance(1, object))
        self.assertTrue(isinstance("x", object))
        self.assertTrue(isinstance([], object))
        self.assertTrue(isinstance(None, object))

    def test_subclass_relations(self):
        self.assertTrue(issubclass(bool, int))
        self.assertTrue(issubclass(int, object))
        self.assertTrue(issubclass(IndexError, LookupError))
        self.assertTrue(issubclass(LookupError, Exception))
        self.assertTrue(issubclass(Exception, BaseException))


class BytesSliceTest(unittest.TestCase):
    def test_slice(self):
        self.assertEqual(b"abcdef"[1:4], b"bcd")
        self.assertEqual(b"abcdef"[::2], b"ace")
        self.assertEqual(b"abcdef"[::-1], b"fedcba")

    def test_bytearray_slice(self):
        self.assertEqual(bytearray(b"abc")[1:], bytearray(b"bc"))


class DictCtorTest(unittest.TestCase):
    def test_copy(self):
        d = {"a": 1, "b": 2}
        e = dict(d)
        self.assertEqual(e, d)
        e["c"] = 3
        self.assertNotIn("c", d)

    def test_from_pairs(self):
        self.assertEqual(dict([("a", 1), ("b", 2)]), {"a": 1, "b": 2})

    def test_from_kwargs(self):
        self.assertEqual(dict(x=1, y=2), {"x": 1, "y": 2})


class FunctoolsPartialTest(unittest.TestCase):
    def test_kwargs(self):
        import functools
        f = functools.partial(int, base=16)
        self.assertEqual(f("ff"), 255)

    def test_args(self):
        import functools
        f = functools.partial(max, 0)
        self.assertEqual(f(5), 5)
        self.assertEqual(f(-1), 0)


class CallableInstanceTest(unittest.TestCase):
    def test_kwargs_through_call(self):
        class P:
            def __init__(self, fn): self.fn = fn
            def __call__(self, *args, **kwargs):
                return self.fn(*args, **kwargs)
        p = P(int)
        self.assertEqual(p("ff", base=16), 255)


class BytesEqTest(unittest.TestCase):
    def test_bytes_eq(self):
        self.assertEqual(b"abc", b"abc")
        self.assertNotEqual(b"abc", b"abd")
        self.assertEqual((256).to_bytes(2, "big"), b"\x01\x00")


unittest.main(globals())
