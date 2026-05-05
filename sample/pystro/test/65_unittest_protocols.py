import unittest


class IntMethodTest(unittest.TestCase):
    def test_bit_length(self):
        self.assertEqual((255).bit_length(), 8)
        self.assertEqual((0).bit_length(), 0)
        self.assertEqual((1).bit_length(), 1)
        self.assertEqual((-256).bit_length(), 9)
        self.assertEqual((2**100).bit_length(), 101)

    def test_bit_count(self):
        self.assertEqual((0).bit_count(), 0)
        self.assertEqual((0b101).bit_count(), 2)
        self.assertEqual((-7).bit_count(), 3)

    def test_to_from_bytes(self):
        self.assertEqual((256).to_bytes(2, "big"), b"\x01\x00")
        self.assertEqual((256).to_bytes(2, "little"), b"\x00\x01")
        self.assertEqual(int.from_bytes(b"\x01\x00", "big"), 256)
        self.assertEqual(int.from_bytes(b"\x00\x01", "little"), 256)

    def test_underscore(self):
        self.assertEqual(int("1_000_000"), 1000000)
        self.assertEqual(int("0xff_ff", 16), 65535)


class FloatMethodTest(unittest.TestCase):
    def test_is_integer(self):
        self.assertTrue((3.0).is_integer())
        self.assertFalse((3.14).is_integer())

    def test_hex(self):
        # roundtrip
        self.assertEqual(float.fromhex((1.5).hex()), 1.5)
        self.assertEqual(float.fromhex("0x1.8p+0"), 1.5)


class ComplexMethodTest(unittest.TestCase):
    def test_conjugate(self):
        self.assertEqual(complex(3, 4).conjugate(), complex(3, -4))

    def test_abs(self):
        self.assertEqual(abs(complex(3, 4)), 5.0)


class TypeIdentityTest(unittest.TestCase):
    def test_type_names(self):
        self.assertEqual(type(None).__name__, "NoneType")
        self.assertEqual(type(print).__name__, "builtin_function_or_method")
        self.assertEqual(type(slice(1)).__name__, "slice")
        self.assertEqual(type(NotImplemented).__name__, "NotImplementedType")
        self.assertEqual(type(Ellipsis).__name__, "ellipsis")
        self.assertEqual(type(lambda: 1).__name__, "function")


class SliceTest(unittest.TestCase):
    def test_slice_attrs(self):
        s = slice(1, 5, 2)
        self.assertEqual(s.start, 1)
        self.assertEqual(s.stop, 5)
        self.assertEqual(s.step, 2)


class RangeSeqTest(unittest.TestCase):
    def test_len(self):
        self.assertEqual(len(range(10)), 10)
        self.assertEqual(len(range(0, 10, 2)), 5)
        self.assertEqual(len(range(10, 0, -1)), 10)

    def test_subscript(self):
        self.assertEqual(range(10)[5], 5)
        self.assertEqual(range(10)[-1], 9)
        self.assertEqual(range(0, 10, 2)[2], 4)

    def test_attrs(self):
        r = range(1, 10, 2)
        self.assertEqual(r.start, 1)
        self.assertEqual(r.stop, 10)
        self.assertEqual(r.step, 2)


class TupleMethodTest(unittest.TestCase):
    def test_count(self):
        self.assertEqual((1, 2, 3, 2, 1).count(2), 2)
        self.assertEqual((1, 2, 3).count(99), 0)

    def test_index(self):
        self.assertEqual((1, 2, 3).index(2), 1)
        with self.assertRaises(ValueError):
            (1, 2, 3).index(99)


class ListMethodTest(unittest.TestCase):
    def test_index_range(self):
        self.assertEqual([1, 2, 3, 2, 1].index(2, 2), 3)
        with self.assertRaises(ValueError):
            [1, 2].index(2, 2)

    def test_count(self):
        self.assertEqual([1, 2, 1, 1].count(1), 3)


class DictMethodTest(unittest.TestCase):
    def test_fromkeys(self):
        self.assertEqual(dict.fromkeys(["a", "b", "c"]), {"a": None, "b": None, "c": None})
        self.assertEqual(dict.fromkeys(["a", "b"], 0), {"a": 0, "b": 0})

    def test_or_assign(self):
        d = {"a": 1}
        d |= {"b": 2}
        self.assertEqual(d, {"a": 1, "b": 2})

    def test_reversed(self):
        d = {"a": 1, "b": 2, "c": 3}
        self.assertEqual(list(reversed(d)), ["c", "b", "a"])


class FrozensetOpTest(unittest.TestCase):
    def test_union(self):
        self.assertEqual(frozenset([1, 2]) | frozenset([2, 3]), frozenset([1, 2, 3]))

    def test_intersection(self):
        self.assertEqual(frozenset([1, 2]) & frozenset([2, 3]), frozenset([2]))

    def test_difference(self):
        self.assertEqual(frozenset([1, 2, 3]) - frozenset([2]), frozenset([1, 3]))

    def test_symmetric_difference(self):
        self.assertEqual(frozenset([1, 2]) ^ frozenset([2, 3]), frozenset([1, 3]))


class StrMethodTest(unittest.TestCase):
    def test_startswith_tuple(self):
        self.assertTrue("abc".startswith(("a", "x")))
        self.assertFalse("abc".startswith(("z", "y")))

    def test_endswith_tuple(self):
        self.assertTrue("abc".endswith(("z", "c")))
        self.assertFalse("abc".endswith(("x", "y")))

    def test_rpartition(self):
        self.assertEqual("a,b,c".rpartition(","), ("a,b", ",", "c"))
        self.assertEqual("abc".rpartition(","), ("", "", "abc"))

    def test_rsplit(self):
        self.assertEqual("a,b,c,d".rsplit(",", 2), ["a,b", "c", "d"])

    def test_format_map(self):
        self.assertEqual("{x}".format_map({"x": 99}), "99")

    def test_translate(self):
        self.assertEqual("abc".translate(str.maketrans("ab", "AB")), "ABc")
        self.assertEqual("abc".translate(str.maketrans("ab", "AB", "c")), "AB")


class ExceptionHierarchyTest(unittest.TestCase):
    def test_baseexception(self):
        self.assertTrue(issubclass(Exception, BaseException))
        self.assertTrue(issubclass(SystemExit, BaseException))
        self.assertTrue(issubclass(KeyboardInterrupt, BaseException))
        self.assertTrue(issubclass(GeneratorExit, BaseException))

    def test_lookup_error(self):
        self.assertTrue(issubclass(IndexError, LookupError))
        self.assertTrue(issubclass(KeyError, LookupError))

    def test_arithmetic_error(self):
        self.assertTrue(issubclass(ZeroDivisionError, ArithmeticError))
        self.assertTrue(issubclass(OverflowError, ArithmeticError))

    def test_unicode_is_value(self):
        self.assertTrue(issubclass(UnicodeError, ValueError))


class StopIterationValueTest(unittest.TestCase):
    def test_gen_return(self):
        def g():
            yield 1
            return 99
        gi = g()
        self.assertEqual(next(gi), 1)
        try:
            next(gi)
            self.fail("should have stopped")
        except StopIteration as e:
            self.assertEqual(e.value, 99)

    def test_explicit(self):
        try:
            raise StopIteration("hello")
        except StopIteration as e:
            self.assertEqual(e.value, "hello")


class IterSentinelTest(unittest.TestCase):
    def test_basic(self):
        counter = [0]
        def f():
            counter[0] += 1
            return counter[0]
        self.assertEqual(list(iter(f, 5)), [1, 2, 3, 4])


class GenexpScopeTest(unittest.TestCase):
    def test_genexp_in_lambda(self):
        f = lambda: list(x * 2 for x in [1, 2, 3])
        self.assertEqual(f(), [2, 4, 6])

    def test_listcomp_in_lambda(self):
        f = lambda: [x + 1 for x in [10, 20, 30]]
        self.assertEqual(f(), [11, 21, 31])


unittest.main(globals())
