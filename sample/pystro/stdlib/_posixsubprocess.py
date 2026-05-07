# pystro stub for `_posixsubprocess` (CPython subprocess C extension).
# pystro doesn't fork/exec; subprocess support is best-effort.

def fork_exec(*args, **kwargs):
    raise OSError("subprocess: not supported in pystro")


def cloexec_pipe():
    return (-1, -1)
