# Metaclass + super proxy tests.

import unittest


class SuperProxyTest(unittest.TestCase):
    def test_super_as_value(self):
        class A:
            def f(self): return "A"
        class B(A):
            def f(self):
                s = super()
                return s.f() + "B"
        self.assertEqual(B().f(), "AB")

    def test_super_explicit_as_value(self):
        class A:
            def f(self): return "A"
        class B(A):
            def f(self):
                s = super(B, self)
                return s.f() + "B"
        self.assertEqual(B().f(), "AB")

    def test_super_chain(self):
        class A:
            def f(self): return ["A"]
        class B(A):
            def f(self):
                return super().f() + ["B"]
        class C(B):
            def f(self):
                return super().f() + ["C"]
        self.assertEqual(C().f(), ["A", "B", "C"])


class MetaclassTest(unittest.TestCase):
    def test_marker_via_meta(self):
        class Meta(type):
            def __new__(mcs, name, bases, attrs):
                attrs["created_by"] = "Meta"
                return type(name, bases, attrs)

        class Foo(metaclass=Meta):
            pass
        self.assertEqual(Foo.created_by, "Meta")

    def test_method_injection(self):
        class AddMethod(type):
            def __new__(mcs, name, bases, attrs):
                attrs["greet"] = lambda self: "hi from " + name
                return type(name, bases, attrs)

        class Greeter(metaclass=AddMethod):
            pass
        self.assertEqual(Greeter().greet(), "hi from Greeter")

    def test_registration(self):
        REGISTRY = []
        class Register(type):
            def __new__(mcs, name, bases, attrs):
                cls = type(name, bases, attrs)
                REGISTRY.append(cls.__name__)
                return cls

        class A(metaclass=Register):
            pass
        class B(metaclass=Register):
            pass
        self.assertIn("A", REGISTRY)
        self.assertIn("B", REGISTRY)


unittest.main(globals())
