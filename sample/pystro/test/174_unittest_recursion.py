import unittest
import sys


class RecursionLimitTest(unittest.TestCase):
    def test_default_limit(self):
        self.assertGreaterEqual(sys.getrecursionlimit(), 100)

    def test_raises_recursion_error(self):
        def deep(n):
            return deep(n + 1)
        with self.assertRaises(RecursionError):
            deep(0)

    def test_set_get(self):
        original = sys.getrecursionlimit()
        try:
            sys.setrecursionlimit(50)
            self.assertEqual(sys.getrecursionlimit(), 50)
        finally:
            sys.setrecursionlimit(original)

    def test_low_limit_triggers(self):
        original = sys.getrecursionlimit()
        try:
            sys.setrecursionlimit(20)
            def deep(n):
                return deep(n + 1)
            with self.assertRaises(RecursionError):
                deep(0)
        finally:
            sys.setrecursionlimit(original)


unittest.main(globals())
