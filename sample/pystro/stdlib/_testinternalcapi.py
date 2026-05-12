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


def compiler_codegen(*args, **kwargs):
    raise NotImplementedError("compiler_codegen")


def compiler_clean_doc(s):
    return s


def optimize_cfg(*args, **kwargs):
    raise NotImplementedError("optimize_cfg")


def assemble_code_object(*args, **kwargs):
    raise NotImplementedError("assemble_code_object")


def get_getpath_codeobject():
    return None


def perf_map_state_teardown():
    pass


def write_perf_map_entry(*args, **kwargs):
    pass


def perf_map_state_init(*args, **kwargs):
    pass


def perf_trampoline_set_persist_after_fork_count():
    return 0


def perf_trampoline_set_persist_after_fork():
    pass


__all__ = ["get_recursion_depth", "get_optimizer", "set_optimizer",
           "compiler_codegen", "compiler_clean_doc", "optimize_cfg",
           "assemble_code_object", "get_getpath_codeobject",
           "perf_map_state_teardown", "perf_trampoline_set_persist_after_fork"]
