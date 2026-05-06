import unittest


class ExceptionAttrsTest(unittest.TestCase):
    def test_traceback_attr_exists(self):
        try:
            1/0
        except ZeroDivisionError as e:
            # Whether or not it's None, the attribute must exist.
            self.assertTrue(hasattr(e, "__traceback__"))

    def test_cause(self):
        try:
            try:
                raise ValueError("A")
            except ValueError as inner:
                raise RuntimeError("B") from inner
        except RuntimeError as e:
            self.assertIsInstance(e.__cause__, ValueError)
            self.assertTrue(e.__suppress_context__)

    def test_implicit_context(self):
        try:
            try:
                raise ValueError("inner")
            except:
                raise RuntimeError("outer")
        except RuntimeError as e:
            self.assertIsInstance(e.__context__, ValueError)

    def test_from_none_suppresses(self):
        try:
            try:
                raise ValueError("inner")
            except:
                raise RuntimeError("outer") from None
        except RuntimeError as e:
            self.assertIsNone(e.__cause__)
            self.assertTrue(e.__suppress_context__)


class StopIterValueTest(unittest.TestCase):
    def test_value(self):
        def g():
            yield 1
            return "ret val"
        gi = g()
        next(gi)
        try:
            next(gi)
        except StopIteration as e:
            self.assertEqual(e.value, "ret val")


unittest.main(globals())
