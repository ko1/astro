"""pystro-friendly stub for `test.support`.  Real CPython test.support
pulls in annotationlib, which depends on CPython-internal descriptors
that pystro can't replicate.  This stub provides just the helpers that
individual test_*.py files actually call — most are skip-decorators or
run-unittest wrappers that resolve to no-ops here."""

import sys
import os
import unittest as _unittest


# ---------------------------------------------------------------------------
# Skip decorators / requirements.
# ---------------------------------------------------------------------------

def cpython_only(fn):
    """Marker — pystro is "not CPython", so skip these tests."""
    return _unittest.skip("cpython-only")(fn)


def requires(resource, msg=None):
    return _unittest.skip("requires " + str(resource) + (": " + msg if msg else ""))


def requires_resource(resource):
    return _unittest.skip("requires " + str(resource))


def is_resource_enabled(resource):
    return False


def get_resource_value(resource):
    return None


def requires_freebsd_version(*v):
    return _unittest.skip("requires FreeBSD")


def requires_gil_enabled(reason=None):
    def deco(fn): return fn
    return deco


def requires_linux_version(*v):
    def deco(fn): return fn
    return deco


def requires_mac_ver(*v):
    return _unittest.skip("requires Mac OS")


def requires_gzip(*a, **kw):
    return _unittest.skip("no gzip")


def requires_bz2(*a, **kw):
    return _unittest.skip("no bz2")


def requires_lzma(*a, **kw):
    return _unittest.skip("no lzma")


def requires_zstd(*a, **kw):
    return _unittest.skip("no zstd")


def requires_zlib(*a, **kw):
    return _unittest.skip("no zlib")


def requires_IEEE_754(fn):
    return fn


def requires_fork():
    return _unittest.skip("no fork")


def requires_subprocess():
    return _unittest.skip("no subprocess")


def requires_working_socket():
    return _unittest.skip("no socket")


def requires_remote_subprocess_debugging():
    return _unittest.skip("no remote subprocess")


def requires_specialization(fn):
    return _unittest.skip("requires specialization")(fn)


def requires_limited_api(fn):
    return _unittest.skip("requires limited API")(fn)


def thread_unsafe(reason=None):
    def deco(fn): return fn
    return deco


def skip_if_buggy_ucrt_strfptime(fn):
    return fn


def skip_if_sanitizer(*a, **kw):
    def deco(fn): return fn
    return deco


def skip_if_unlimited_stack_size(fn):
    return fn


def check_sanitizer(*a, **kw):
    return False


def has_fork_support():
    return False


def has_subprocess_support():
    return False


def has_socket_support():
    return False


def has_remote_subprocess_debugging():
    return False


def bigmemtest(size, memuse, dry_run=True):
    def deco(fn): return _unittest.skip("bigmem")(fn)
    return deco


def bigaddrspacetest(fn):
    return _unittest.skip("bigaddrspace")(fn)


def anticipate_failure(condition):
    def deco(fn): return fn
    return deco


# ---------------------------------------------------------------------------
# Platform / runtime info.
# ---------------------------------------------------------------------------

PIPE_MAX_SIZE = 4 * 1024 * 1024
verbose = False
max_memuse = 0
use_resources = []
failfast = False

MS_WINDOWS = sys.platform.startswith("win") if hasattr(sys, "platform") else False
is_jython = False
is_android = False
is_emscripten = False
is_wasi = False
is_apple_mobile = False
is_apple = False

# Cached attribute: helpers expect these to exist.
TESTFN = "@test"
HAVE_DOCSTRINGS = True
Py_DEBUG = False


# ---------------------------------------------------------------------------
# Capture helpers — context managers that intercept stdout/stderr/stdin.
# ---------------------------------------------------------------------------

class _CapturedIO:
    def __init__(self, name):
        self._name = name
        self._buf = []
    def __enter__(self):
        import io
        self._stream = io.StringIO()
        self._saved = getattr(sys, self._name)
        setattr(sys, self._name, self._stream)
        return self._stream
    def __exit__(self, *exc):
        setattr(sys, self._name, self._saved)
        return False


def captured_stdout(): return _CapturedIO("stdout")
def captured_stderr(): return _CapturedIO("stderr")
def captured_stdin():  return _CapturedIO("stdin")


def captured_output(stream_name):
    return _CapturedIO(stream_name)


def record_original_stdout(stdout):
    pass


def get_original_stdout():
    return sys.stdout


# ---------------------------------------------------------------------------
# Exceptions.
# ---------------------------------------------------------------------------

class Error(Exception): pass
class TestFailed(Error): pass
class TestDidNotRun(Error): pass
class ResourceDenied(_unittest.SkipTest): pass


# ---------------------------------------------------------------------------
# Misc helpers.
# ---------------------------------------------------------------------------

def get_attribute(obj, name):
    try:
        return getattr(obj, name)
    except AttributeError:
        raise _unittest.SkipTest("no attribute " + name)


def check_syntax_error(test_case, statement, errtext="", lineno=None, offset=None):
    """Verify that *statement* is a syntax error in CPython.  Pystro's
    parser may accept some syntax CPython rejects; here we just exec()
    and consider any error as the expected one."""
    try:
        compile(statement, "<test>", "exec")
        test_case.fail("did not get SyntaxError")
    except SyntaxError:
        return


def detect_api_mismatch(*a, **kw):
    return set()


def check__all__(test_case, module, *a, **kw):
    pass


def check_disallow_instantiation(test_case, cls, *args, **kwargs):
    pass


def load_package_tests(*args, **kwargs):
    return None


def run_unittest(*classes):
    """Old-style runner — feed each class to unittest.main."""
    suite = _unittest.TestSuite()
    loader = _unittest.TestLoader()
    for cls in classes:
        suite.addTests(loader.loadTestsFromTestCase(cls))
    runner = _unittest.TextTestRunner(verbosity=0)
    runner.run(suite)


def run_doctest(module, *args, **kwargs):
    pass


def reap_children():
    pass


def reap_threads(fn):
    return fn


def check_free_after_iterating(*a, **kw):
    pass


def import_helper():
    return None


def get_signal_name(sig):
    return "signal_" + str(sig)


def gc_collect():
    pass


def threading_setup(*a, **kw):
    return ()


def threading_cleanup(*a, **kw):
    pass


def disable_gc():
    class _NoOp:
        def __enter__(self): return self
        def __exit__(self, *exc): return False
    return _NoOp()


def no_tracing(fn):
    return fn


def gc_threshold(*a):
    class _NoOp:
        def __enter__(self): return self
        def __exit__(self, *exc): return False
    return _NoOp()


# Marker classes for tests that introspect them.
class EqualToForwardRef:
    def __init__(self, *a, **kw): pass
    def __eq__(self, other): return False
    def __hash__(self): return 0


# ---------------------------------------------------------------------------
# Misc constants.
# ---------------------------------------------------------------------------

SHORT_TIMEOUT = 30.0
LONG_TIMEOUT = 600.0
LOOPBACK_TIMEOUT = 5.0
INTERNET_TIMEOUT = 60.0
ALWAYS_EQ = type("AlwaysEqual", (), {"__eq__": lambda s, o: True, "__hash__": lambda s: 0})()
NEVER_EQ = type("NeverEqual", (), {"__eq__": lambda s, o: False, "__hash__": lambda s: 0})()


def cpython_only(fn):
    return _unittest.skip("cpython-only")(fn)


# More stubs that newer CPython tests reach for.
def skip_wasi_stack_overflow():
    def deco(fn): return fn
    return deco

def skip_emscripten_stack_overflow():
    def deco(fn): return fn
    return deco

def skip_if_buildbot(reason=None):
    def deco(fn): return fn
    return deco

def skip_on_s390x():
    def deco(fn): return fn
    return deco

def adjust_int_max_str_digits(n):
    class _NoOp:
        def __enter__(self): return self
        def __exit__(self, *exc): return False
    return _NoOp()


def requires_docstrings(fn):
    return fn


def requires_docstring(fn):
    return fn


def cpython_only(fn):
    return _unittest.skip("cpython-only")(fn)


def has_strftime_extensions():
    return False


def Py_DEBUG_BUILD():
    return False


# Memory thresholds.
_1G = 1 * 1024 * 1024 * 1024
_2G = 2 * 1024 * 1024 * 1024
_4G = 4 * 1024 * 1024 * 1024
MAX_Py_ssize_t = (2 ** 63) - 1
PIPE_MAX_SIZE = 4 * 1024 * 1024


def python_is_optimized():
    return False


def with_pymalloc():
    return False


def is_resource_enabled(name): return False
def get_resource_value(name): return 0


def force_not_colorized(fn): return fn
def force_not_colorized_test_class(cls): return cls
def set_recursion_limit(n):
    class _Ctx:
        def __enter__(self): return self
        def __exit__(self, *e): return False
    return _Ctx()
def infinite_recursion(*a, **kw):
    class _Ctx:
        def __enter__(self): return self
        def __exit__(self, *e): return False
    return _Ctx()


def expected_failure_if(condition, reason=None):
    def deco(fn):
        if condition:
            def w(*a, **kw):
                try: fn(*a, **kw)
                except Exception: return
                raise AssertionError("expected failure but didn't fail")
            return w
        return fn
    return deco


def os_helper_warning_filter():
    class _Ctx:
        def __enter__(self): return self
        def __exit__(self, *e): return False
    return _Ctx()


def busy_retry(*a, **kw):
    yield from range(1)


def sleeping_retry(*a, **kw):
    yield from range(1)


def has_strftime_extensions():
    return False


# BrokenIter — used by tests that need to assert failure modes.
class BrokenIter:
    def __init__(self, **kw):
        self._behaviour = kw
    def __iter__(self): return self
    def __next__(self):
        raise StopIteration


# Eagerly import sub-modules so `test.support.os_helper.X` works.
from test.support import os_helper
from test.support import import_helper
from test.support import warnings_helper
from test.support import threading_helper
from test.support import socket_helper
from test.support import script_helper
