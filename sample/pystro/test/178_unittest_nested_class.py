import unittest


class NestedClassTest(unittest.TestCase):
    def test_simple_nested(self):
        class Outer:
            class Inner:
                value = 42
        self.assertEqual(Outer.Inner.value, 42)
        self.assertEqual(Outer.Inner.__name__, "Inner")

    def test_nested_instance(self):
        class Container:
            class Item:
                def __init__(self, x): self.x = x
        item = Container.Item(7)
        self.assertEqual(item.x, 7)

    def test_nested_with_method(self):
        class O:
            class I:
                def hi(self): return "hi from inner"
        self.assertEqual(O.I().hi(), "hi from inner")

    def test_nested_used_in_outer(self):
        class O:
            class I:
                X = 1
            def get_x(self):
                return self.I.X
        self.assertEqual(O().get_x(), 1)


unittest.main(globals())
