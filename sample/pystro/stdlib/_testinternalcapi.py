"""pystro stub for the CPython internal capi test helper."""


import sys


def get_recursion_depth(): return 0


def get_optimizer():
    class _Opt:
        def get_count(self): return 0
    return _Opt()


def set_optimizer(opt): pass


def compiler_clean_doc(s): return s


# Various probes CPython tests do.
def get_object_count(): return 0
def get_type_cache_entries(): return []
def assert_python_compatibility_with_self_inspecting_optimizer(): pass


SIZEOF_PYGC_HEAD = 0


__all__ = ["get_recursion_depth", "get_optimizer", "set_optimizer"]
