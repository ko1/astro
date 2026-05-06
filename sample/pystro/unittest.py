# pystro stdlib `unittest` (minimal — enough to run small CPython-style
# test cases).
#
# Supports:
#   class C(TestCase):
#       def test_foo(self): self.assertEqual(1+1, 2)
#   unittest.main()
#
# Each test method whose name starts with "test_" is run in declaration
# order.  Failures are reported but don't stop the whole run.

import sys

_collected_results = [0, 0]   # [passed, failed]


class _AssertionError(AssertionError):
    pass


class TestCase:
    # Default failure handler — just raises so the runner counts it.
    def fail(self, msg=""):
        raise AssertionError(msg or "fail()")

    def assertTrue(self, x, msg=""):
        if not x:
            self.fail(msg or ("assertTrue: got " + repr(x)))

    def assertFalse(self, x, msg=""):
        if x:
            self.fail(msg or ("assertFalse: got " + repr(x)))

    def assertEqual(self, a, b, msg=""):
        if a != b:
            self.fail(msg or (repr(a) + " != " + repr(b)))

    def assertNotEqual(self, a, b, msg=""):
        if a == b:
            self.fail(msg or (repr(a) + " == " + repr(b)))

    def assertIs(self, a, b, msg=""):
        if a is not b:
            self.fail(msg or (repr(a) + " is not " + repr(b)))

    def assertIsNot(self, a, b, msg=""):
        if a is b:
            self.fail(msg or (repr(a) + " is " + repr(b)))

    def assertIsNone(self, x, msg=""):
        if x is not None:
            self.fail(msg or (repr(x) + " is not None"))

    def assertIsNotNone(self, x, msg=""):
        if x is None:
            self.fail(msg or "value is None")

    def assertIn(self, a, b, msg=""):
        if a not in b:
            self.fail(msg or (repr(a) + " not in " + repr(b)))

    def assertNotIn(self, a, b, msg=""):
        if a in b:
            self.fail(msg or (repr(a) + " in " + repr(b)))

    def assertIsInstance(self, obj, cls, msg=""):
        if not isinstance(obj, cls):
            self.fail(msg or (repr(obj) + " is not instance of " + repr(cls)))

    def assertNotIsInstance(self, obj, cls, msg=""):
        if isinstance(obj, cls):
            self.fail(msg or (repr(obj) + " is instance of " + repr(cls)))

    def assertGreater(self, a, b, msg=""):
        if not (a > b):
            self.fail(msg or (repr(a) + " <= " + repr(b)))

    def assertGreaterEqual(self, a, b, msg=""):
        if not (a >= b):
            self.fail(msg or (repr(a) + " < " + repr(b)))

    def assertLess(self, a, b, msg=""):
        if not (a < b):
            self.fail(msg or (repr(a) + " >= " + repr(b)))

    def assertLessEqual(self, a, b, msg=""):
        if not (a <= b):
            self.fail(msg or (repr(a) + " > " + repr(b)))

    def assertAlmostEqual(self, a, b, places=7, msg=""):
        diff = a - b
        if diff < 0: diff = -diff
        thresh = 10.0 ** (-places)
        if diff > thresh:
            self.fail(msg or (repr(a) + " not almost equal " + repr(b)))

    # Context-manager assertRaises: usage —
    #   with self.assertRaises(ValueError):
    #       int("xy")
    def assertRaises(self, exc_cls, *args):
        if len(args) == 0:
            return _AssertRaisesCM(exc_cls)
        # Direct form: assertRaises(exc, callable, *args).
        cm = _AssertRaisesCM(exc_cls)
        with cm:
            args[0](*args[1:])

    def setUp(self):
        pass

    def tearDown(self):
        pass


class _AssertRaisesCM:
    def __init__(self, exc_cls):
        self.exc_cls = exc_cls
        self.exception = None
    def __enter__(self):
        return self
    def __exit__(self, exc_type, exc_value, tb):
        # No exception → assertRaises failed.
        if exc_type is None:
            raise AssertionError(
                str(self.exc_cls) + " not raised")
        # Match (and any subclass).
        if not issubclass(exc_type, self.exc_cls):
            return False     # let it propagate
        self.exception = exc_value
        return True          # suppress


# Run all TestCase subclasses' test_* methods declared in the caller's
# module.  Caller passes globals(), or we use sys.modules trick.
def main(scope=None):
    if scope is None:
        scope = {}

    cases = []
    for name in scope:
        v = scope[name]
        if hasattr(v, "_is_test_case_"):
            cases.append(v)

    passed = 0
    failed = 0
    for cls in cases:
        method_names = []
        for n in dir(cls):
            if n.startswith("test_"):
                method_names.append(n)
        for mn in method_names:
            inst = cls()
            inst.setUp()
            try:
                m = getattr(inst, mn)
                m()
                passed += 1
                print("ok", cls.__name__ if hasattr(cls, "__name__") else "?", mn)
            except Exception as e:
                failed += 1
                print("FAIL", cls.__name__ if hasattr(cls, "__name__") else "?", mn, ":", e)
            inst.tearDown()
    print("---")
    print("passed=" + str(passed) + " failed=" + str(failed))
    return 0 if failed == 0 else 1


# Mark TestCase as test-case-base (for the main() walker).
TestCase._is_test_case_ = True

__all__ = ["TestCase", "main"]
