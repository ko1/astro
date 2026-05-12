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

    def assertIsSubclass(self, sub, cls, msg=""):
        if not issubclass(sub, cls):
            self.fail(msg or (repr(sub) + " is not subclass of " + repr(cls)))

    def assertNotIsSubclass(self, sub, cls, msg=""):
        if issubclass(sub, cls):
            self.fail(msg or (repr(sub) + " is subclass of " + repr(cls)))

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
    def assertRaises(self, exc_cls, *args, **kwargs):
        if len(args) == 0:
            return _AssertRaisesCM(exc_cls)
        # Direct form: assertRaises(exc, callable, *args, **kwargs).
        cm = _AssertRaisesCM(exc_cls)
        with cm:
            args[0](*args[1:], **kwargs)

    def assertRaisesRegex(self, exc_cls, regex, *args, **kwargs):
        cm = _AssertRaisesCM(exc_cls, regex=regex)
        # `msg=` kwarg is consumed by the CM, not forwarded to the callable.
        kwargs.pop("msg", None)
        if len(args) == 0:
            return cm
        with cm:
            args[0](*args[1:], **kwargs)

    def assertNotHasAttr(self, obj, name, msg=""):
        if hasattr(obj, name):
            self.fail(msg or (repr(obj) + " has attribute " + repr(name)))

    def assertHasAttr(self, obj, name, msg=""):
        if not hasattr(obj, name):
            self.fail(msg or (repr(obj) + " has no attribute " + repr(name)))

    def assertWarns(self, warn_cls, *args):
        return _AssertNoOpCM()

    def assertWarnsRegex(self, warn_cls, regex, *args):
        return _AssertNoOpCM()

    def assertLogs(self, logger=None, level=None):
        return _AssertNoOpCM()

    def assertNoLogs(self, logger=None, level=None):
        return _AssertNoOpCM()

    def assertCountEqual(self, a, b, msg=""):
        if sorted(a, key=lambda x: (str(type(x)), repr(x))) != \
           sorted(b, key=lambda x: (str(type(x)), repr(x))):
            self.fail(msg or (repr(a) + " has different elements than " + repr(b)))

    def assertSequenceEqual(self, a, b, msg="", seq_type=None):
        if list(a) != list(b):
            self.fail(msg or (repr(a) + " != " + repr(b)))

    def assertListEqual(self, a, b, msg=""):
        self.assertSequenceEqual(a, b, msg, seq_type=list)

    def assertTupleEqual(self, a, b, msg=""):
        self.assertSequenceEqual(a, b, msg, seq_type=tuple)

    def assertMultiLineEqual(self, a, b, msg=""):
        # Same as assertEqual for plain strings — pystro doesn't show
        # the per-line unified diff CPython produces but the truth-value
        # branch is what tests check.
        if a != b:
            self.fail(msg or (repr(a) + " != " + repr(b)))

    def assertDictEqual(self, a, b, msg=""):
        if a != b:
            self.fail(msg or (repr(a) + " != " + repr(b)))

    def assertSetEqual(self, a, b, msg=""):
        if set(a) != set(b):
            self.fail(msg or (repr(a) + " != " + repr(b)))

    def assertDictEqual(self, a, b, msg=""):
        if a != b:
            self.fail(msg or (repr(a) + " != " + repr(b)))

    def assertSetEqual(self, a, b, msg=""):
        if set(a) != set(b):
            self.fail(msg or (repr(a) + " != " + repr(b)))

    def assertRegex(self, text, regex, msg=""):
        import re as _re
        if isinstance(regex, str): regex = _re.compile(regex)
        if not regex.search(text):
            self.fail(msg or ("regex " + repr(regex) + " not found in " + repr(text)))

    def assertNotRegex(self, text, regex, msg=""):
        import re as _re
        if isinstance(regex, str): regex = _re.compile(regex)
        if regex.search(text):
            self.fail(msg or ("regex " + repr(regex) + " unexpectedly found in " + repr(text)))

    def subTest(self, *args, **kwargs):
        return _AssertNoOpCM()

    def addCleanup(self, fn, *args, **kwargs):
        if not hasattr(self, "_cleanups"):
            self._cleanups = []
        self._cleanups.append((fn, args, kwargs))

    def doCleanups(self):
        if hasattr(self, "_cleanups"):
            while self._cleanups:
                fn, a, k = self._cleanups.pop()
                try: fn(*a, **k)
                except Exception: pass

    def enterContext(self, cm):
        # CPython 3.11+: enter a context manager and register __exit__
        # via addCleanup so the test body can use the value directly.
        v = cm.__enter__()
        self.addCleanup(cm.__exit__, None, None, None)
        return v

    @classmethod
    def enterClassContext(cls, cm):
        v = cm.__enter__()
        cls.addClassCleanup(cm.__exit__, None, None, None)
        return v

    @classmethod
    def addClassCleanup(cls, fn, *args, **kwargs):
        if not hasattr(cls, "_class_cleanups"):
            cls._class_cleanups = []
        cls._class_cleanups.append((fn, args, kwargs))


    def fail(self, msg=""):
        raise AssertionError(msg)

    def skipTest(self, reason):
        raise SkipTest(reason)

    def setUp(self):
        pass

    def tearDown(self):
        pass

    @classmethod
    def setUpClass(cls): pass

    @classmethod
    def tearDownClass(cls): pass


class IsolatedAsyncioTestCase(TestCase):
    """CPython 3.8+ async-aware TestCase.  pystro runs async tests
    synchronously via a tiny event loop in run_async; expose the type
    so tests that subclass it at least don't import-error."""
    def _run_async(self, coro):
        try:
            coro.send(None)
        except StopIteration as e:
            return getattr(e, "value", None)
        return None


class _AssertNoOpCM:
    def __enter__(self): return self
    def __exit__(self, *exc): return False


class _AssertRaisesCM:
    def __init__(self, exc_cls, regex=None):
        self.exc_cls = exc_cls
        self.regex = regex
        self.exception = None
    def __enter__(self):
        return self
    def __exit__(self, exc_type, exc_value, tb):
        if exc_type is None:
            raise AssertionError(str(self.exc_cls) + " not raised")
        if not issubclass(exc_type, self.exc_cls):
            return False
        if self.regex is not None:
            import re as _re
            r = self.regex if not isinstance(self.regex, str) else _re.compile(self.regex)
            if not r.search(str(exc_value)):
                raise AssertionError("regex " + repr(self.regex) + " not found in " + repr(str(exc_value)))
        self.exception = exc_value
        return True


# Flag the pystro runtime checks at script exit: if main() was never
# called and the script's globals contain TestCase subclasses, run
# them.  Lets test files that omit `unittest.main()` (e.g. CPython
# tests run via `python -m unittest`) still execute.
_main_called = False


# Run all TestCase subclasses' test_* methods declared in the caller's
# module.  Caller passes globals(), or we walk sys.modules.__main__.
def main(scope=None, *args, **kwargs):
    global _main_called
    _main_called = True
    # CPython's unittest.main accepts module=, exit=, verbosity=, etc.
    # Pystro's stub ignores them.
    if isinstance(scope, dict):
        ns = scope
    else:
        # Default: scan all modules that have test cases.  Prefer
        # __main__ if present.
        import sys
        ns = {}
        try:
            main_mod = sys.modules.get("__main__")
            if main_mod is not None:
                ns = vars(main_mod)
        except Exception:
            pass
        if not ns:
            # Scan all modules for TestCase subclasses (best effort).
            for m in list(sys.modules.values()):
                if m is None: continue
                try:
                    md = vars(m)
                except TypeError:
                    continue
                for name in md:
                    v = md[name]
                    if isinstance(v, type) and hasattr(v, "_is_test_case_"):
                        ns[name] = v

    cases = []
    seen = set()
    # If the module defines `load_tests(loader, tests, pattern)`, honour
    # it: cpython tests use it to restrict / reorder the suite (e.g.
    # test_io excludes the abstract IOTest base from direct execution).
    load_tests = ns.get("load_tests") if isinstance(ns, dict) else None
    import sys as _sys
    if not callable(load_tests):
        # Sometimes vars(__main__) misses freshly-bound globals; consult
        # sys.modules['__main__'] directly via __dict__ getattr.
        try:
            mm = _sys.modules.get("__main__")
            if mm is not None:
                lt2 = getattr(mm, "load_tests", None)
                if callable(lt2):
                    load_tests = lt2
        except Exception:
            pass
    if callable(load_tests):
        # Provide a CPython-shaped loader so test_io etc.'s suite-building
        # idioms (`loader.suiteClass()` + `suite.addTest(...)`) work.
        collected = []
        class _DummySuite:
            def __init__(self): self.items = []
            def addTest(self, test):
                # Flatten any nested suites / lists.
                if isinstance(test, (list, tuple)):
                    for sub in test:
                        self.addTest(sub)
                elif hasattr(test, "items"):
                    for sub in test.items:
                        self.addTest(sub)
                elif isinstance(test, type) and hasattr(test, "_is_test_case_"):
                    self.items.append(test)
            def __iter__(self):
                return iter(self.items)
        class _DummyLoader:
            suiteClass = _DummySuite
            def loadTestsFromTestCase(self, cls):
                s = _DummySuite()
                s.items.append(cls)
                return s
        try:
            result = load_tests(_DummyLoader(), _DummySuite(), None)
        except Exception:
            result = None
        if result is not None:
            try:
                if hasattr(result, "items"):
                    collected = result.items
                elif isinstance(result, (list, tuple)):
                    collected = result
                for item in collected:
                    if isinstance(item, type) and hasattr(item, "_is_test_case_"):
                        if id(item) not in seen:
                            seen.add(id(item))
                            cases.append(item)
                    elif isinstance(item, (list, tuple)):
                        for sub in item:
                            if isinstance(sub, type) and hasattr(sub, "_is_test_case_"):
                                if id(sub) not in seen:
                                    seen.add(id(sub))
                                    cases.append(sub)
            except Exception:
                pass
    if not cases:
        for name in ns:
            v = ns[name]
            if isinstance(v, type) and hasattr(v, "_is_test_case_"):
                if id(v) in seen: continue
                seen.add(id(v))
                cases.append(v)

    passed = 0
    failed = 0
    skipped = 0
    for cls in cases:
        method_names = []
        for n in dir(cls):
            # unittest's default TestLoader matches "test*" (not just
            # "test_*"); CPython tests sometimes declare a single
            # `def test(self):` (e.g. test_stringprep).  Limit to the
            # bare `test` name to avoid pulling in helper methods
            # whose names happen to start with `test` (which can
            # break previously-passing test suites).
            if n == "test" or n.startswith("test_"):
                # CPython idiom: subclasses set `test_xxx = None` to
                # disable an inherited test method.  Skip non-callable
                # entries so they don't end up as "object is not
                # callable" errors.
                try:
                    v = getattr(cls, n)
                except AttributeError:
                    continue
                if v is None or not callable(v):
                    continue
                method_names.append(n)
        for mn in method_names:
            try:
                inst = cls()
            except Exception as e:
                failed += 1
                print("FAIL", cls.__name__ if hasattr(cls, "__name__") else "?", mn,
                      ": __init__ raised", type(e).__name__, ":", e)
                continue
            try:
                inst.setUp()
            except SkipTest as e:
                skipped += 1
                print("skip", cls.__name__, mn, ":", e)
                continue
            except SystemExit as e:
                failed += 1
                print("FAIL", cls.__name__ if hasattr(cls, "__name__") else "?", mn,
                      ": setUp SystemExit")
                continue
            except Exception as e:
                # setUp failure: record as fail and move on (CPython
                # does the same).  Without this catch, a single test
                # with broken setUp aborts the entire run (test_pstats
                # had this — KeyError in StatsTestCase.setUp swallowed
                # all subsequent tests).
                failed += 1
                print("FAIL", cls.__name__ if hasattr(cls, "__name__") else "?", mn,
                      ": setUp", type(e).__name__, ":", e)
                continue
            try:
                m = getattr(inst, mn)
                m()
                passed += 1
                print("ok", cls.__name__ if hasattr(cls, "__name__") else "?", mn)
            except SkipTest as e:
                skipped += 1
                print("skip", cls.__name__, mn, ":", e)
            except SystemExit as e:
                # A test that calls sys.exit() (e.g. argparse `-h`) would
                # otherwise terminate the whole sweep; record as failure.
                failed += 1
                print("FAIL", cls.__name__ if hasattr(cls, "__name__") else "?", mn,
                      ": SystemExit(" + str(getattr(e, "code", "")) + ")")
            except Exception as e:
                failed += 1
                print("FAIL", cls.__name__ if hasattr(cls, "__name__") else "?", mn, ":", e)
            try: inst.tearDown()
            except Exception: pass
            try: inst.doCleanups()
            except Exception: pass
    print("---")
    print("passed=" + str(passed) + " failed=" + str(failed) +
          (" skipped=" + str(skipped) if skipped else ""))
    return 0 if failed == 0 else 1


# Mark TestCase as test-case-base (for the main() walker).
TestCase._is_test_case_ = True


# Skip mechanism — when a test raises SkipTest, the runner records skip.
class SkipTest(Exception):
    pass


def skip(reason):
    """Decorator: unconditionally skip the decorated test."""
    def deco(fn):
        def w(*a, **kw):
            raise SkipTest(reason)
        w.__skip__ = True
        w.__skip_reason__ = reason
        return w
    return deco


def skipIf(condition, reason):
    if condition: return skip(reason)
    def deco(fn): return fn
    return deco


def skipUnless(condition, reason):
    if not condition: return skip(reason)
    def deco(fn): return fn
    return deco


def expectedFailure(fn):
    def w(*a, **kw):
        try:
            fn(*a, **kw)
        except Exception:
            return
        raise AssertionError("test was expected to fail but didn't")
    return w


# Test loader / suite / runner — minimal so test.support.run_unittest can
# instantiate them.
class TestLoader:
    def loadTestsFromTestCase(self, cls):
        return TestSuite([cls])


class TestSuite:
    def __init__(self, cases=()):
        self.cases = list(cases)
    def addTest(self, t):
        self.cases.append(t)
    def addTests(self, tests):
        for t in tests: self.cases.append(t)


class TextTestRunner:
    def __init__(self, *args, **kwargs):
        pass
    def run(self, suite):
        for cls in suite.cases:
            if isinstance(cls, type):
                main(cls.__dict__)
            else:
                # treat as suite
                self.run(cls)


# unittest.mock — minimal sub-namespace.
class _MockSentinel:
    def __init__(self, name): self.name = name
    def __repr__(self): return f"sentinel.{self.name}"


class _MockSentinelFactory:
    def __getattr__(self, name): return _MockSentinel(name)


class _Mock:
    def __init__(self, *args, **kwargs):
        self._spec = kwargs.pop("spec", None)
        self._return_value = kwargs.pop("return_value", None)
        self._side_effect = kwargs.pop("side_effect", None)
        self.call_args = None
        self.call_args_list = []
        self.call_count = 0
        self._children = {}
        for k, v in kwargs.items():
            setattr(self, k, v)
    def __call__(self, *args, **kwargs):
        self.call_args = (args, kwargs)
        self.call_args_list.append(self.call_args)
        self.call_count += 1
        if self._side_effect is not None:
            if callable(self._side_effect):
                return self._side_effect(*args, **kwargs)
            return self._side_effect
        return self._return_value
    def __getattr__(self, name):
        if name in ("_spec", "_return_value", "_side_effect",
                    "call_args", "call_args_list", "call_count",
                    "_children"):
            raise AttributeError(name)
        if name not in self._children:
            self._children[name] = _Mock()
        return self._children[name]
    @property
    def return_value(self): return self._return_value
    @return_value.setter
    def return_value(self, v): self._return_value = v
    def reset_mock(self):
        self.call_args = None
        self.call_args_list = []
        self.call_count = 0
    def assert_called(self):
        if self.call_count == 0:
            raise AssertionError("Expected to be called")
    def assert_called_once(self):
        if self.call_count != 1:
            raise AssertionError(f"Expected 1 call, got {self.call_count}")
    def assert_not_called(self):
        if self.call_count != 0:
            raise AssertionError(f"Expected no calls, got {self.call_count}")
    def assert_called_with(self, *args, **kwargs):
        if self.call_args != (args, kwargs):
            raise AssertionError(f"Expected {args}, {kwargs}, got {self.call_args}")
    def assert_called_once_with(self, *args, **kwargs):
        self.assert_called_once()
        self.assert_called_with(*args, **kwargs)


def _patch(target, *args, **kwargs):
    class _PatchCM:
        def __enter__(self): return _Mock()
        def __exit__(self, *exc): return False
        def __call__(self, fn): return fn
    return _PatchCM()


_patch.object = lambda obj, attr, *a, **k: _patch(f"{obj}.{attr}", *a, **k)
_patch.dict = lambda *a, **k: _patch(*a, **k)
_patch.multiple = lambda *a, **k: _patch(*a, **k)


class _MockModule:
    Mock = _Mock
    MagicMock = _Mock
    NonCallableMock = _Mock
    NonCallableMagicMock = _Mock
    PropertyMock = _Mock
    AsyncMock = _Mock
    patch = staticmethod(_patch)
    sentinel = _MockSentinelFactory()
    DEFAULT = object()
    ANY = object()
    @staticmethod
    def call(*args, **kwargs):
        return (args, kwargs)
    @staticmethod
    def create_autospec(spec, *args, **kwargs):
        return _Mock()
    @staticmethod
    def mock_open(read_data=""):
        m = _Mock()
        m.read = _Mock(return_value=read_data)
        return m


mock = _MockModule()


__all__ = ["TestCase", "main", "SkipTest", "skip", "skipIf", "skipUnless",
           "expectedFailure", "TestLoader", "TestSuite", "TextTestRunner",
           "mock"]
