import unittest


class DirMROTest(unittest.TestCase):
    def test_inherited(self):
        class A:
            def m1(self): pass
        class B(A):
            def m2(self): pass
        attrs = sorted([n for n in dir(B) if not n.startswith("_")])
        self.assertEqual(attrs, ["m1", "m2"])

    def test_instance(self):
        class A:
            def m1(self): pass
        class B(A):
            def m2(self): pass
        b = B()
        b.x = 5
        attrs = sorted([n for n in dir(b) if not n.startswith("_")])
        self.assertIn("m1", attrs)
        self.assertIn("m2", attrs)
        self.assertIn("x", attrs)

    def test_overridden(self):
        class A:
            def f(self): pass
        class B(A):
            def f(self): pass
        # Should appear once.
        attrs = [n for n in dir(B) if n == "f"]
        self.assertEqual(len(attrs), 1)


unittest.main(globals())
