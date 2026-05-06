import unittest


class InlineSuiteTest(unittest.TestCase):
    def test_inline_def(self):
        # Single-line def with multiple statements separated by ';'.
        def f(): a = 1; b = 2; return a + b
        self.assertEqual(f(), 3)

    def test_inline_method(self):
        class C:
            def __enter__(self): self.x = 1; return self
            def __exit__(self, *a): self.x = 0
        with C() as c:
            self.assertEqual(c.x, 1)
        self.assertEqual(c.x, 0)


unittest.main(globals())
