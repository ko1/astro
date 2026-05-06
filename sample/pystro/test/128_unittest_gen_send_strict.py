import unittest


class GenSendStrictTest(unittest.TestCase):
    def test_send_unstarted_with_none(self):
        # send(None) on unstarted gen primes it.
        def g():
            x = yield 1
            yield x
        it = g()
        self.assertEqual(it.send(None), 1)

    def test_send_unstarted_non_none(self):
        # send(non-None) on unstarted gen → TypeError.
        def g():
            yield 1
        it = g()
        with self.assertRaises(TypeError):
            it.send(99)


class StopIterValueOnExhaustionTest(unittest.TestCase):
    def test_value_after_normal_exhaust(self):
        def g():
            yield 1
        it = g()
        list(it)
        try:
            next(it)
        except StopIteration as e:
            self.assertIsNone(e.value)

    def test_value_with_return(self):
        def g():
            yield 1
            return "done"
        it = g()
        try:
            list(it)
            next(it)
        except StopIteration as e:
            self.assertEqual(e.value, "done")


unittest.main(globals())
