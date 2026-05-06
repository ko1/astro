import unittest
import types


class TypesIdentityTest(unittest.TestCase):
    def test_method_type(self):
        class C:
            def m(self): pass
        self.assertIsInstance(C().m, types.MethodType)

    def test_function_type(self):
        def f(): pass
        self.assertIsInstance(f, types.FunctionType)

    def test_lambda_type(self):
        f = lambda: 0
        self.assertIsInstance(f, types.FunctionType)

    def test_generator_type(self):
        def g():
            yield 1
        self.assertIsInstance(g(), types.GeneratorType)

    def test_module_type(self):
        import os
        self.assertIsInstance(os, types.ModuleType)

    def test_none_type(self):
        self.assertIsInstance(None, types.NoneType)


unittest.main(globals())
