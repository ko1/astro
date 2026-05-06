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


# IEEE 754 double float info — pystro uses native C double everywhere.
class _FloatInfo(tuple):
    @property
    def max(self): return self[0]
    @property
    def max_exp(self): return self[1]
    @property
    def max_10_exp(self): return self[2]
    @property
    def min(self): return self[3]
    @property
    def min_exp(self): return self[4]
    @property
    def min_10_exp(self): return self[5]
    @property
    def dig(self): return self[6]
    @property
    def mant_dig(self): return self[7]
    @property
    def epsilon(self): return self[8]
    @property
    def radix(self): return self[9]
    @property
    def rounds(self): return self[10]


float_info = _FloatInfo((1.7976931348623157e308, 1024, 308,
                          2.2250738585072014e-308, -1021, -307,
                          15, 53,
                          2.220446049250313e-16,
                          2, 1))


class _IntInfo(tuple):
    @property
    def bits_per_digit(self): return self[0]
    @property
    def sizeof_digit(self): return self[1]
    @property
    def default_max_str_digits(self): return self[2]
    @property
    def str_digits_check_threshold(self): return self[3]


int_info = _IntInfo((30, 4, 4300, 640))


# Limit on integer-to-string conversion length (CPython 3.11+).
def get_int_max_str_digits(): return 4300
def set_int_max_str_digits(n): pass


# Path / install info that some libs reach for.
base_prefix = prefix
base_exec_prefix = exec_prefix
real_prefix = prefix
flags = type("Flags", (), {
    "debug": 0, "inspect": 0, "interactive": 0, "optimize": 0,
    "dont_write_bytecode": 1, "no_user_site": 0, "no_site": 0,
    "ignore_environment": 0, "verbose": 0, "bytes_warning": 0,
    "quiet": 0, "hash_randomization": 0, "isolated": 0, "dev_mode": 0,
    "utf8_mode": 1, "warn_default_encoding": 0, "safe_path": 0,
    "int_max_str_digits": 4300,
})()
abiflags = ""
dont_write_bytecode = True
warnoptions = []
intern = lambda s: s
getrecursionlimit = lambda: __pystro_get_recursion_limit__()
setrecursionlimit = lambda n: __pystro_set_recursion_limit__(n)
getsizeof = lambda obj, default=0: 64
getrefcount = lambda obj: 1
gettrace = lambda: None
settrace = lambda fn: None
getprofile = lambda: None
setprofile = lambda fn: None
exc_info = lambda: (None, None, None)
exception = lambda: None
displayhook = lambda v: print(repr(v)) if v is not None else None
excepthook = lambda *a: None
ps1 = ">>> "
ps2 = "... "

def get_coroutine_origin_tracking_depth(): return 0
def set_coroutine_origin_tracking_depth(n): pass
def is_finalizing(): return False
def _is_gil_enabled(): return True
def _is_immortal(obj): return False
def audit(event, *args): pass
def addaudithook(hook): pass

def _getframe(depth=0):
    class _Frame:
        f_globals = {}
        f_locals = {}
        f_lineno = 0
        f_code = type("Code", (), {"co_name": "<frame>", "co_filename": "<frame>"})()
        f_back = None
    return _Frame()

class _ExceptionInfo:
    pass

def get_asyncgen_hooks(): return type("_Hooks", (), {"firstiter": None, "finalizer": None})()
def set_asyncgen_hooks(*a, **kw): pass

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
