"""R17 final compat checks: function annotations, PEP 585 generics,
class.__class__, get_type_hints, get_origin."""
import unittest


class FunctionAnnotationsTest(unittest.TestCase):
    def test_param_annotations(self):
        def f(x: int, y: str = "d") -> bool: return True
        ann = f.__annotations__
        self.assertEqual(ann["x"], int)
        self.assertEqual(ann["y"], str)
        self.assertEqual(ann["return"], bool)

    def test_get_type_hints_function(self):
        from typing import get_type_hints
        def f(x: int, y: float) -> str: return ""
        hints = get_type_hints(f)
        self.assertEqual(hints["x"], int)
        self.assertEqual(hints["y"], float)
        self.assertEqual(hints["return"], str)

    def test_signature_carries_annotation(self):
        import inspect
        def f(x: int, y: str = "d") -> bool: return True
        sig = inspect.signature(f)
        self.assertEqual(sig.parameters["x"].annotation, int)
        self.assertEqual(sig.parameters["y"].annotation, str)
        self.assertEqual(sig.return_annotation, bool)


class PEP585GenericTest(unittest.TestCase):
    def test_list_int(self):
        # list[int] returns the list class itself in pystro.
        self.assertIs(list[int], list)
        self.assertIs(dict[str, int], dict)
        self.assertIs(tuple[int, ...], tuple)
        self.assertIs(set[int], set)
        self.assertIs(frozenset[int], frozenset)
        self.assertIs(type[int], type)

    def test_generic_in_annotation(self):
        def f(x: list[int]) -> dict[str, int]: return {}
        self.assertEqual(f.__annotations__["x"], list)
        self.assertEqual(f.__annotations__["return"], dict)


class ClassClassTest(unittest.TestCase):
    def test_class_of_class_is_type(self):
        self.assertIs(int.__class__, type)
        self.assertIs(list.__class__, type)
        class C: pass
        self.assertIs(C.__class__, type)

    def test_metaclass_is_class_of(self):
        class M(type): pass
        class C(metaclass=M): pass
        self.assertIs(C.__class__, M)


class TypingExtraTest(unittest.TestCase):
    def test_get_origin(self):
        from typing import get_origin, List
        # PEP 585 alias: pystro returns None.
        self.assertIsNone(get_origin(list[int]))
        # typing.List: returns list.
        self.assertIs(get_origin(List[int]), list)


unittest.main(globals())
