# Adapted from CPython test_exceptions.py.

import unittest


class ExceptionTest(unittest.TestCase):
    def test_basic(self):
        try:
            raise ValueError("oops")
        except ValueError as e:
            caught = True
        self.assertTrue(caught)

    def test_message(self):
        try:
            raise ValueError("hello")
        except ValueError as e:
            self.assertEqual(str(e), "hello")

    def test_class_hierarchy(self):
        # ValueError is a subclass of Exception.
        try:
            raise ValueError("v")
        except Exception as e:
            self.assertIsInstance(e, ValueError)
            self.assertIsInstance(e, Exception)

    def test_multi_except(self):
        def f(n):
            if n == 1: raise ValueError("v")
            if n == 2: raise TypeError("t")
            if n == 3: raise KeyError("k")
            return "ok"
        results = []
        for i in [0, 1, 2, 3]:
            try:
                results.append(f(i))
            except ValueError:
                results.append("V")
            except TypeError:
                results.append("T")
            except KeyError:
                results.append("K")
        self.assertEqual(results, ["ok", "V", "T", "K"])

    def test_tuple_except(self):
        try:
            raise ValueError("v")
        except (TypeError, ValueError) as e:
            self.assertIsInstance(e, ValueError)

    def test_else(self):
        ran = []
        try:
            ran.append("try")
        except Exception:
            ran.append("except")
        else:
            ran.append("else")
        self.assertEqual(ran, ["try", "else"])

    def test_finally(self):
        ran = []
        try:
            try:
                ran.append("try")
                raise ValueError()
            finally:
                ran.append("finally")
        except ValueError:
            ran.append("except")
        self.assertEqual(ran, ["try", "finally", "except"])

    def test_finally_with_return(self):
        ran = []
        def f():
            try:
                ran.append("try")
                return "try-return"
            finally:
                ran.append("finally")
        result = f()
        self.assertEqual(result, "try-return")
        self.assertEqual(ran, ["try", "finally"])

    def test_nested(self):
        try:
            try:
                raise ValueError("inner")
            except TypeError:
                self.fail("won't catch")
        except ValueError as e:
            self.assertEqual(str(e), "inner")

    def test_reraise(self):
        def relay():
            try:
                raise ValueError("orig")
            except ValueError:
                raise

        try:
            relay()
        except ValueError as e:
            self.assertEqual(str(e), "orig")

    def test_raise_from(self):
        try:
            try:
                raise ValueError("orig")
            except ValueError as e:
                raise TypeError("wrapped") from e
        except TypeError as e:
            self.assertEqual(str(e), "wrapped")
            self.assertEqual(str(e.__cause__), "orig")

    def test_user_exception(self):
        class MyErr(Exception):
            pass
        try:
            raise MyErr("custom")
        except MyErr as e:
            self.assertEqual(str(e), "custom")

    def test_user_exception_init(self):
        class MyErr(Exception):
            def __init__(self, code, msg):
                super().__init__(msg)
                self.code = code
        try:
            raise MyErr(42, "boom")
        except MyErr as e:
            self.assertEqual(e.code, 42)
            self.assertEqual(str(e), "boom")

    def test_zero_division(self):
        try:
            x = 1 / 0
        except ZeroDivisionError:
            pass
        else:
            self.fail("no exception")

    def test_attr_error(self):
        class P: pass
        try:
            P().nope
        except AttributeError:
            pass
        else:
            self.fail("no exception")

    def test_index_error(self):
        try:
            [1, 2][10]
        except IndexError:
            pass
        else:
            self.fail("no exception")

    def test_key_error(self):
        try:
            {"a": 1}["x"]
        except KeyError:
            pass
        else:
            self.fail("no exception")

    def test_assert(self):
        try:
            assert False, "boom"
        except AssertionError as e:
            self.assertEqual(str(e), "boom")

    def test_traceback_attr(self):
        # __traceback__ attribute is a list of frame names in pystro.
        def deep():
            raise RuntimeError("at deep")
        def mid():
            deep()
        try:
            mid()
        except RuntimeError as e:
            tb = e.__traceback__
            self.assertIn("mid", tb)
            self.assertIn("deep", tb)


unittest.main(globals())
