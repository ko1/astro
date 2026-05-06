import unittest


class ClassLevelDescriptorTest(unittest.TestCase):
    def test_get_called_on_class_access(self):
        # CPython invokes desc.__get__(None, owner) when accessing a
        # descriptor as a class attribute (non-data descriptors).
        class Desc:
            def __set_name__(self, owner, name):
                self.owner = owner
                self.name = name
            def __get__(self, obj, owner=None):
                return ("get", obj, owner)

        class A:
            x = Desc()
        result = A.x
        self.assertEqual(result, ("get", None, A))

    def test_get_called_on_instance(self):
        class Desc:
            def __get__(self, obj, owner=None):
                return ("instance", obj is not None)
        class A:
            x = Desc()
        self.assertEqual(A().x, ("instance", True))

    def test_data_descriptor_via_property(self):
        class C:
            @property
            def v(self): return 42
        # Accessing C.v at class level returns the property object,
        # not the value.  (CPython behaviour for properties.)
        self.assertEqual(type(C.v).__name__, "property")
        self.assertEqual(C().v, 42)


unittest.main(globals())
