import unittest


class SuperInitSubclassTest(unittest.TestCase):
    def test_chain(self):
        events = []
        class Root:
            def __init_subclass__(cls, **kw):
                super().__init_subclass__(**kw)
                events.append(("root", cls.__name__))

        class Mid(Root):
            def __init_subclass__(cls, **kw):
                super().__init_subclass__(**kw)
                events.append(("mid", cls.__name__))

        class Leaf(Mid): pass
        self.assertIn(("root", "Mid"), events)
        self.assertIn(("root", "Leaf"), events)
        self.assertIn(("mid", "Leaf"), events)

    def test_with_kwargs(self):
        observed = []
        class Tagged:
            def __init_subclass__(cls, *, tag=None, **kw):
                super().__init_subclass__(**kw)
                observed.append((cls.__name__, tag))

        class A(Tagged, tag="A"): pass
        class B(Tagged, tag="B"): pass
        self.assertEqual(observed, [("A", "A"), ("B", "B")])


unittest.main(globals())
