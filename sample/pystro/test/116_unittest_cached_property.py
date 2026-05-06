import unittest
import functools


class CachedPropertyTest(unittest.TestCase):
    def test_basic(self):
        log = []
        class C:
            def __init__(self, n): self.n = n
            @functools.cached_property
            def expensive(self):
                log.append("compute")
                return self.n * 2
        c = C(5)
        self.assertEqual(c.expensive, 10)
        self.assertEqual(c.expensive, 10)
        self.assertEqual(log, ["compute"])  # only computed once


class AbsBoolTest(unittest.TestCase):
    def test_basic(self):
        self.assertEqual(abs(True), 1)
        self.assertEqual(abs(False), 0)


unittest.main(globals())
