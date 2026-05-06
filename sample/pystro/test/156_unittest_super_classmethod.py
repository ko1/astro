import unittest


class SuperClassmethodTest(unittest.TestCase):
    def test_basic(self):
        class A:
            @classmethod
            def hi(cls): return "A"
        class B(A):
            @classmethod
            def hi(cls): return super().hi() + "+B"
        self.assertEqual(B.hi(), "A+B")

    def test_threelevel(self):
        class A:
            @classmethod
            def name(cls): return cls.__name__ + "(A)"
        class B(A):
            @classmethod
            def name(cls): return super().name() + "(B)"
        class C(B):
            @classmethod
            def name(cls): return super().name() + "(C)"
        self.assertEqual(C.name(), "C(A)(B)(C)")

    def test_super_via_instance(self):
        class A:
            @classmethod
            def hi(cls): return "Ainst"
        class B(A):
            @classmethod
            def hi(cls): return super().hi() + "+inst"
        self.assertEqual(B().hi(), "Ainst+inst")


class SuperStaticmethodTest(unittest.TestCase):
    def test_basic(self):
        class A:
            @staticmethod
            def hi(): return "stat-A"
        class B(A):
            @staticmethod
            def hi(): return A.hi() + "+B"  # super().hi() doesn't work for staticmethod consistently
        self.assertEqual(B.hi(), "stat-A+B")


unittest.main(globals())
