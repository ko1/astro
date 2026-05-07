# pystro stdlib `warnings` — print-based stub.
import sys

_filters = []
_silent = False


def warn(message, category=None, stacklevel=1):
    if _silent:
        return
    cat_name = "UserWarning"
    if category is not None and hasattr(category, "__name__"):
        cat_name = category.__name__
    print(cat_name + ": " + str(message))


def warn_explicit(message, category, filename, lineno, *args, **kwargs):
    warn(message, category)


def filterwarnings(action, message="", category=None, module="", lineno=0, append=False):
    _filters.append((action, message, category, module, lineno))


def simplefilter(action, category=None, lineno=0, append=False):
    if action == "ignore":
        global _silent
        _silent = True


def resetwarnings():
    _filters.clear()
    global _silent
    _silent = False


class catch_warnings:
    def __init__(self, record=False):
        self.record = record
    def __enter__(self):
        return [] if self.record else None
    def __exit__(self, *args):
        return False


# Standard warning categories.
class Warning(Exception): pass
class UserWarning(Warning): pass
class DeprecationWarning(Warning): pass
class PendingDeprecationWarning(Warning): pass
class SyntaxWarning(Warning): pass
class RuntimeWarning(Warning): pass
class FutureWarning(Warning): pass
class ImportWarning(Warning): pass
class UnicodeWarning(Warning): pass
class BytesWarning(Warning): pass
class ResourceWarning(Warning): pass
