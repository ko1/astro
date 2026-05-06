"""pystro stub for `_imp` (low-level import machinery)."""

import sys


def acquire_lock(): pass
def release_lock(): pass
def lock_held(): return False


def is_builtin(name):
    return name in sys.builtin_module_names if hasattr(sys, "builtin_module_names") else False


def is_frozen(name):
    return False


def is_frozen_package(name):
    return False


def get_frozen_object(name):
    raise ImportError(f"No frozen object {name}")


def init_frozen(name):
    raise ImportError(f"No frozen module {name}")


def init_builtin(name):
    return None


def get_magic():
    return b"\x00\x00\x00\x00"


def extension_suffixes():
    return [".so"]


def source_hash(key, source):
    return b"\x00" * 8


def check_hash_based_pycs():
    return "default"


def _override_frozen_modules_for_tests(value):
    pass


def _frozen_module_names():
    return []


def _override_multi_interp_extensions_check(value):
    return 0


__all__ = ["acquire_lock", "release_lock", "lock_held", "is_builtin",
           "is_frozen", "is_frozen_package", "get_frozen_object",
           "init_frozen", "init_builtin", "get_magic",
           "extension_suffixes", "source_hash", "check_hash_based_pycs"]
