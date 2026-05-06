import unittest


class GenCloseRaiseTest(unittest.TestCase):
    def test_close_normal(self):
        # Normal close — should not raise.
        def g():
            try:
                yield 1
            finally:
                pass
        gi = g()
        next(gi)
        gi.close()  # silent

    def test_close_finally_raises(self):
        # If gen's finally raises an exception, close() propagates.
        def g():
            try:
                yield 1
            finally:
                raise ValueError("from finally")
        gi = g()
        next(gi)
        with self.assertRaises(ValueError):
            gi.close()

    def test_close_swallows_stop_iteration(self):
        def g():
            yield 1
        gi = g()
        next(gi)
        gi.close()  # silent — gen exhausted naturally during close

    def test_close_already_closed(self):
        def g():
            yield 1
        gi = g()
        next(gi)
        gi.close()
        gi.close()  # idempotent


unittest.main(globals())
