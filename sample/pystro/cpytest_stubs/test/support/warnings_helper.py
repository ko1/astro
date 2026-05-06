"""Stub for test.support.warnings_helper."""
import warnings


class WarningsRecorder:
    def __init__(self):
        self.warnings = []
    def __enter__(self): return self
    def __exit__(self, *exc): return False


def check_warnings(*filters, quiet=True):
    return WarningsRecorder()


def check_no_warnings(test_case):
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


__all__ = ["check_warnings", "check_no_warnings", "ignore_warnings",
           "WarningsRecorder"]
