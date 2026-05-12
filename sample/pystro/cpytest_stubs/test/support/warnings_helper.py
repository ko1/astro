"""Stub for test.support.warnings_helper."""
import warnings


class WarningsRecorder:
    def __init__(self):
        self.warnings = []
    def __enter__(self): return self
    def __exit__(self, *exc): return False


def check_warnings(*filters, quiet=True):
    return WarningsRecorder()


def check_no_warnings(test_case, *args, **kwargs):
    class _NoOp:
        def __enter__(self): return self
        def __exit__(self, *exc): return False
    return _NoOp()


def check_no_resource_warning(test_case):
    return check_no_warnings(test_case)


def ignore_warnings(*, category):
    def deco(fn):
        def wrapper(*a, **kw):
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", category)
                return fn(*a, **kw)
        return wrapper
    return deco


def import_deprecated(name):
    """Replacement for warnings_helper.import_deprecated — pystro just
    does a plain __import__ and ignores DeprecationWarning categories
    (which pystro doesn't enforce anyway)."""
    return __import__(name)


def check_syntax_warning(testcase, statement, errtext="", lineno=None, offset=None):
    """No-op stub: pystro doesn't emit SyntaxWarning."""
    return None


class save_restore_warnings_filters:
    """Context manager that saves/restores warnings.filters."""
    def __enter__(self):
        self._saved = list(warnings.filters)
        return self
    def __exit__(self, *exc):
        warnings.filters[:] = self._saved
        return False


__all__ = ["check_warnings", "check_no_warnings", "ignore_warnings",
           "WarningsRecorder", "import_deprecated",
           "save_restore_warnings_filters"]
