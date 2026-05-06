# pystro stdlib `sys` (minimal).

argv = __pystro_argv__()

# `sys.path` — module search path.
path = ["", __pystro_getcwd__()]

# Version info — pystro reports as Python 3.12 to satisfy version-gated
# stdlib code; pystro's own version is in version_info[3..].
class _VersionInfo(tuple):
    """Python's sys.version_info: tuple-like with named fields."""
    @property
    def major(self): return self[0]
    @property
    def minor(self): return self[1]
    @property
    def micro(self): return self[2]
    @property
    def releaselevel(self): return self[3]
    @property
    def serial(self): return self[4]
version = "3.12.0 (pystro 0.1)"
version_info = _VersionInfo((3, 12, 0, "final", 0))
hexversion = 0x030C00F0
api_version = 1013

# Platform identifier — pystro is Linux-only in practice.
platform = "linux"
maxsize = 9223372036854775807    # 2**63 - 1
maxunicode = 1114111
byteorder = "little"
prefix = "/usr"
exec_prefix = "/usr"
executable = "pystro"
implementation_name = "pystro"

# Stream sentinels — these are file-like objects; pystro doesn't
# expose them yet, so we use simple stand-ins.
class _StdStream:
    def __init__(self, fd):
        self.fd = fd
    def write(self, s):
        if self.fd == 1:
            print(s, end="")
        else:
            __pystro_stderr_write__(s) if "__pystro_stderr_write__" in dir() else print(s, end="")
    def flush(self):
        pass
    def close(self):
        pass
    @property
    def closed(self):
        return False
    def isatty(self):
        return False

stdin  = _StdStream(0)
stdout = _StdStream(1)
stderr = _StdStream(2)

modules = __pystro_modules__()

def exit(*args):
    # CPython raises SystemExit so `try: sys.exit()` is catchable.  Only
    # uncaught SystemExit actually terminates the process.  No args =>
    # SystemExit() (e.code is None, exit status 0).
    if not args:
        raise SystemExit()
    raise SystemExit(args[0])


def exc_info():
    # Return (type, value, traceback) for the currently handled exception,
    # or (None, None, None) if none.
    e = __pystro_current_exc__()
    if e is None:
        return (None, None, None)
    return (type(e), e, getattr(e, "__traceback__", None))

def getrecursionlimit():
    return __pystro_get_recursion_limit__()

def setrecursionlimit(n):
    __pystro_set_recursion_limit__(n)
    return None

def getsizeof(obj, default=0):
    return 64    # arbitrary fixed estimate

def getrefcount(obj):
    return 2     # pystro uses GC, not refcount; placeholder

def intern(s):
    return s

def settrace(tracefunc):
    pass

def gettrace():
    return None

def displayhook(value):
    if value is not None:
        print(repr(value))

def excepthook(exc_type, exc_value, tb):
    print(repr(exc_value))

__all__ = ["argv", "path", "version", "version_info", "platform",
           "maxsize", "maxunicode", "byteorder", "prefix", "exec_prefix",
           "executable", "implementation_name",
           "stdin", "stdout", "stderr", "modules",
           "exit", "exc_info", "getrecursionlimit", "setrecursionlimit",
           "getsizeof", "getrefcount", "intern", "settrace", "gettrace",
           "displayhook", "excepthook"]
