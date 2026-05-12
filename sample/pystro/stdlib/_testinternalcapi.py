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


def write_perf_map_entry(code_addr, code_size, entry_name):
    """CPython writes to /tmp/perf-<pid>.map.  Stub: do the same so
    perf-map self-tests pass without depending on the real C hook."""
    import os
    path = f"/tmp/perf-{os.getpid()}.map"
    line = f"{code_addr:x} {code_size:x} {entry_name}\n"
    try:
        with open(path, "a") as f:
            f.write(line)
        return 0
    except OSError:
        return -1


def perf_map_state_init(*args, **kwargs):
    pass


def perf_trampoline_set_persist_after_fork_count():
    return 0


def perf_trampoline_set_persist_after_fork():
    pass


def normalize_path(path):
    """CPython exposes Python/fileutils.c's _Py_normpath via this hook.
    Stub: implement in pure Python with matching collapse rules.

    POSIX: a path beginning with exactly two slashes (`//`) has
    implementation-defined meaning; preserve the leading `//`.  Three
    or more leading slashes collapse to one.
    """
    if not path:
        return "."
    # Detect prefix.
    if path.startswith("//") and not path.startswith("///"):
        prefix = "//"
        body = path[2:]
    elif path.startswith("/"):
        prefix = "/"
        body = path.lstrip("/")
    else:
        prefix = ""
        body = path
    abs_ = prefix != ""
    out = []
    for p in body.split("/"):
        if p == "" or p == ".":
            continue
        if p == "..":
            if out and out[-1] != "..":
                out.pop()
            elif not abs_:
                out.append(p)
            continue
        out.append(p)
    res = "/".join(out)
    if abs_:
        return prefix + res
    return res or "."


__all__ = ["get_recursion_depth", "get_optimizer", "set_optimizer",
           "compiler_codegen", "compiler_clean_doc", "optimize_cfg",
           "assemble_code_object", "get_getpath_codeobject",
           "perf_map_state_teardown", "perf_trampoline_set_persist_after_fork",
           "normalize_path"]
