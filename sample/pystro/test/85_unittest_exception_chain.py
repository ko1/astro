import unittest


class ExceptionChainTest(unittest.TestCase):
    def test_implicit_context(self):
        try:
            try:
                raise ValueError("v")
            except ValueError:
                raise RuntimeError("r")
        except RuntimeError as e:
            self.assertIsNotNone(e.__context__)
            self.assertIsInstance(e.__context__, ValueError)

    def test_explicit_cause(self):
        try:
            try:
                raise ValueError("orig")
            except ValueError as v:
                raise RuntimeError("wrapped") from v
        except RuntimeError as e:
            self.assertIsInstance(e.__cause__, ValueError)
            self.assertTrue(getattr(e, "__suppress_context__", False))

    def test_from_none(self):
        try:
            try:
                raise ValueError("a")
            except ValueError:
                raise RuntimeError("b") from None
        except RuntimeError as e:
            self.assertIsNone(e.__cause__)
            self.assertTrue(getattr(e, "__suppress_context__", False))

    def test_no_context_outside_handler(self):
        try:
            raise ValueError("v")
        except ValueError as e:
            ctx = e.__context__ if hasattr(e, "__context__") else None
            self.assertIsNone(ctx)

    def test_context_chain_three_deep(self):
        try:
            try:
                try:
                    raise KeyError("k")
                except KeyError:
                    raise ValueError("v")
            except ValueError:
                raise RuntimeError("r")
        except RuntimeError as e:
            self.assertIsInstance(e.__context__, ValueError)
            self.assertIsInstance(e.__context__.__context__, KeyError)


unittest.main(globals())
