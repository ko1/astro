import unittest


class ExceptionDunderTest(unittest.TestCase):
    def test_cause_is_none_by_default(self):
        try:
            raise ValueError("x")
        except ValueError as e:
            self.assertIsNone(e.__cause__)

    def test_traceback_attr_exists(self):
        try:
            raise ValueError("x")
        except ValueError as e:
            self.assertTrue(hasattr(e, "__traceback__"))

    def test_suppress_context_default_false(self):
        try:
            raise ValueError("x")
        except ValueError as e:
            self.assertFalse(e.__suppress_context__)

    def test_context_implicit(self):
        try:
            try:
                raise ValueError("a")
            except:
                raise RuntimeError("b")
        except RuntimeError as e:
            self.assertIsInstance(e.__context__, ValueError)
            self.assertIsNone(e.__cause__)

    def test_cause_explicit(self):
        try:
            try:
                raise ValueError("a")
            except ValueError as inner:
                raise RuntimeError("b") from inner
        except RuntimeError as e:
            self.assertIsInstance(e.__cause__, ValueError)
            self.assertTrue(e.__suppress_context__)


unittest.main(globals())
