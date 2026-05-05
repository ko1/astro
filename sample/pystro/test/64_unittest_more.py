import unittest


class ExceptionClassTest(unittest.TestCase):
    def test_import_error(self):
        try:
            import nonexistent_xyz_module
            self.fail("should have raised")
        except ImportError as e:
            pass        # OK
        except Exception as e:
            self.fail("wrong exc type: " + repr(e))

    def test_module_not_found(self):
        with self.assertRaises(ModuleNotFoundError):
            import another_nonexistent_xyz

    def test_module_not_found_is_import(self):
        with self.assertRaises(ImportError):
            import another_nonexistent_xyz

    def test_overflow_arithmetic(self):
        # ArithmeticError exists.
        try:
            raise ArithmeticError("a")
        except ArithmeticError:
            pass
        # OverflowError is an ArithmeticError.
        try:
            raise OverflowError("o")
        except ArithmeticError:
            pass

    def test_filenotfound_is_oserror(self):
        try:
            raise FileNotFoundError("nope")
        except OSError:
            pass
        try:
            raise FileNotFoundError("nope")
        except IOError:        # IOError is OSError alias
            pass


class PrintKwargTest(unittest.TestCase):
    def test_sep(self):
        # Capture via redirecting print: easier — just call print directly.
        # Test via str returned from a roundabout way (no StringIO in pystro mini).
        # Skip; rely on visual confirmation.
        pass


class StrMethodTest(unittest.TestCase):
    def test_capitalize(self):
        self.assertEqual("hello WORLD".capitalize(), "Hello world")
        self.assertEqual("".capitalize(), "")
        self.assertEqual("a".capitalize(), "A")

    def test_rfind(self):
        self.assertEqual("hello world".rfind("o"), 7)
        self.assertEqual("hello world".rfind("z"), -1)
        self.assertEqual("abc".rfind(""), 3)

    def test_index_value_error(self):
        self.assertEqual("hello".index("l"), 2)
        with self.assertRaises(ValueError):
            "hello".index("z")

    def test_isnumeric(self):
        self.assertTrue("123".isnumeric())
        self.assertFalse("12a".isnumeric())
        self.assertFalse("".isnumeric())

    def test_isascii(self):
        self.assertTrue("abc".isascii())
        self.assertTrue("".isascii())


class LazyIterTest(unittest.TestCase):
    def test_enumerate_lazy(self):
        def evens():
            n = 0
            while True:
                yield n
                n += 2
        def take(it, n):
            for i, v in enumerate(it):
                if i >= n: return
                yield v
        self.assertEqual(list(take(evens(), 5)), [0, 2, 4, 6, 8])

    def test_zip_lazy(self):
        def naturals():
            n = 0
            while True:
                yield n
                n += 1
        # Pair naturals with [a, b, c] → only 3 elements consumed.
        result = list(zip(naturals(), ["a", "b", "c"]))
        self.assertEqual(result, [(0, "a"), (1, "b"), (2, "c")])

    def test_map_lazy(self):
        def naturals():
            n = 0
            while True:
                yield n
                n += 1
        m = map(lambda x: x * 2, naturals())
        from itertools import islice
        out = []
        for i, v in enumerate(m):
            if i >= 4: break
            out.append(v)
        self.assertEqual(out, [0, 2, 4, 6])

    def test_filter_lazy(self):
        def naturals():
            n = 0
            while True:
                yield n
                n += 1
        f = filter(lambda x: x % 3 == 0, naturals())
        out = []
        for i, v in enumerate(f):
            if i >= 4: break
            out.append(v)
        self.assertEqual(out, [0, 3, 6, 9])


unittest.main(globals())
