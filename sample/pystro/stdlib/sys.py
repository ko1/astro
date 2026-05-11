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

# Built-in (statically linked) module names.  CPython's os.py / abc.py
# / importlib gate behavior on this list.  Include modules pystro
# provides as built-in stubs (no .py file).
builtin_module_names = (
    "_imp", "_warnings", "_weakref", "_io", "_collections_abc",
    "_collections", "_abc", "_signal", "_thread", "_codecs", "_string",
    "_ast", "_locale", "_socket", "_contextvars", "_tracemalloc",
    "_symtable", "_lsprof", "_multibytecodec", "_opcode",
    "atexit", "errno", "faulthandler", "gc", "marshal", "posix",
    "pwd", "grp", "select", "sys", "time",
)

# sys.implementation — namespace object with name / version / hexversion
# / cache_tag.  CPython tests gate on `sys.implementation.name == 'cpython'`
# and on version_info; pystro reports as 3.12 to satisfy version-gated paths.
class _Implementation:
    name = "pystro"     # honest identity — code that wants CPython-only
                        # paths gates on this and skips
    cache_tag = "pystro-0"
    _multiarch = "x86_64-linux-gnu"
    def __repr__(self):
        return f"namespace(name={self.name!r}, version={self.version!r})"
implementation = _Implementation()
implementation.version = version_info
implementation.hexversion = hexversion

# CPython adds these in 3.9+: sysconfig / pathlib / venv all read them.
platlibdir = "lib"
exec_prefix = "/usr/local"
prefix = "/usr/local"
base_prefix = prefix
base_exec_prefix = exec_prefix
abiflags = ""
maxsize = (1 << 63) - 1
maxunicode = 0x10FFFF
float_repr_style = "short"
api_version = 1013

# `sys.flags` namespace — CPython tests check `sys.flags.optimize` etc.
class _Flags:
    debug = 0
    inspect = 0
    interactive = 0
    optimize = 0
    dont_write_bytecode = 0
    no_user_site = 0
    no_site = 0
    ignore_environment = 0
    verbose = 0
    bytes_warning = 0
    quiet = 0
    hash_randomization = 1
    isolated = 0
    dev_mode = False
    utf8_mode = 1
    safe_path = 0
    int_max_str_digits = 4300

flags = _Flags()


# `sys.float_info` — IEEE 754 double constants.
class _FloatInfo:
    max = 1.7976931348623157e+308
    max_exp = 1024
    max_10_exp = 308
    min = 2.2250738585072014e-308
    min_exp = -1021
    min_10_exp = -307
    dig = 15
    mant_dig = 53
    epsilon = 2.220446049250313e-16
    radix = 2
    rounds = 1

float_info = _FloatInfo()


# `sys.int_info` — long-int internals (used by serializers).
class _IntInfo:
    bits_per_digit = 30
    sizeof_digit = 4
    default_max_str_digits = 4300
    str_digits_check_threshold = 640

int_info = _IntInfo()


# `sys.hash_info` — hash randomization parameters.
class _HashInfo:
    width = 64
    modulus = (1 << 61) - 1
    inf = 314159
    nan = 0
    imag = 1000003
    algorithm = "siphash24"
    hash_bits = 64
    seed_bits = 128
    cutoff = 0

hash_info = _HashInfo()


def getrecursionlimit():
    return 1000


def setrecursionlimit(n):
    return None


def getswitchinterval():
    return 0.005


def setswitchinterval(n):
    return None


def get_int_max_str_digits():
    return 4300


def set_int_max_str_digits(n):
    return None


def intern(s):
    return s


def is_finalizing():
    return False

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
    def fileno(self):
        return self.fd
    @property
    def buffer(self):
        # CPython exposes a binary buffer underlying the text stream;
        # tests probe it but rarely actually use it.  Self is close
        # enough — methods that take str will still accept str input.
        return self
    @property
    def encoding(self):
        return "utf-8"
    @property
    def errors(self):
        return "strict"
    @property
    def name(self):
        return ("<stdin>", "<stdout>", "<stderr>")[self.fd]
    @property
    def mode(self):
        return "r" if self.fd == 0 else "w"
    def writable(self):
        return self.fd != 0
    def readable(self):
        return self.fd == 0
    def seekable(self):
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
def getfilesystemencoding(): return "utf-8"
def getfilesystemencodeerrors(): return "surrogateescape"
def getdefaultencoding(): return "utf-8"
def setdefaultencoding(enc): pass
def _is_gil_enabled(): return True
def _is_immortal(obj): return False
def audit(event, *args): pass
def addaudithook(hook): pass


# sys.monitoring (PEP 669, Python 3.12+) — instrumentation API.  pystro
# doesn't run a tracing loop; expose stubs that no-op.
class _Monitoring:
    DEBUGGER_ID = 0
    COVERAGE_ID = 1
    PROFILER_ID = 2
    OPTIMIZER_ID = 5
    PROFILER_TOOL_ID = PROFILER_ID
    DEBUGGER_TOOL_ID = DEBUGGER_ID
    class events:
        BRANCH = 1; CALL = 2; C_RAISE = 4; C_RETURN = 8; EXCEPTION_HANDLED = 16
        INSTRUCTION = 32; JUMP = 64; LINE = 128; PY_RESUME = 256; PY_RETURN = 512
        PY_START = 1024; PY_THROW = 2048; PY_UNWIND = 4096; PY_YIELD = 8192
        RAISE = 16384; RERAISE = 32768; STOP_ITERATION = 65536; NO_EVENTS = 0
    MISSING = object()
    @staticmethod
    def use_tool_id(tool_id, name): pass
    @staticmethod
    def free_tool_id(tool_id): pass
    @staticmethod
    def get_tool(tool_id): return None
    @staticmethod
    def register_callback(tool_id, event, func): return None
    @staticmethod
    def get_events(tool_id): return 0
    @staticmethod
    def set_events(tool_id, event_set): pass
    @staticmethod
    def get_local_events(tool_id, code): return 0
    @staticmethod
    def set_local_events(tool_id, code, event_set): pass
    @staticmethod
    def restart_events(): pass


monitoring = _Monitoring()

def _getframe(depth=0):
    class _Frame:
        def __init__(self):
            self.f_globals = {}
            self.f_locals = {}
            self.f_lineno = 0
            self.f_code = type("Code", (), {"co_name": "<frame>", "co_filename": "<frame>"})()
            self.f_back = None
            self.f_lasti = 0
            self.f_trace = None
            self.f_trace_lines = True
            self.f_trace_opcodes = False
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

# CPython exposes original (un-patched) hooks as __excepthook__ etc.
__excepthook__ = excepthook
__displayhook__ = displayhook
__breakpointhook__ = lambda *a, **kw: None
__unraisablehook__ = lambda *a, **kw: None

# Import system internals — empty / stubs.
meta_path = []
path_hooks = []
path_importer_cache = {}


# Original streams: CPython preserves the un-redirected stdio as
# __stdout__ / __stderr__ / __stdin__ so test frameworks can swap
# back if they monkeypatched them.  Pystro never redirects, so they
# all alias the live streams.
__stdout__ = stdout
__stderr__ = stderr
__stdin__ = stdin


# CPython 3.11+: sys._base_executable / _base_executable_realpath are
# used by venv tests to find the "underlying" interpreter.  Pystro
# is single-binary, so reuse `executable`.
_base_executable = executable
base_executable = executable
base_prefix = prefix
base_exec_prefix = exec_prefix
flags = type("flags", (), {
    "debug": 0,
    "inspect": 0,
    "interactive": 0,
    "optimize": 0,
    "dont_write_bytecode": 0,
    "no_user_site": 0,
    "no_site": 0,
    "ignore_environment": 0,
    "verbose": 0,
    "bytes_warning": 0,
    "quiet": 0,
    "hash_randomization": 0,
    "isolated": 0,
    "dev_mode": False,
    "utf8_mode": 1,
    "safe_path": False,
    "int_max_str_digits": 4300,
    "warn_default_encoding": 0,
    "warn_invalid_extension_byte": 0,
})()
warnoptions = []
hexversion = 0x030C0000  # 3.12.0
api_version = 1013
abiflags = ""
dont_write_bytecode = False
pycache_prefix = None

# Build / interpreter metadata referenced by some CPython tests.  Pystro
# isn't CPython; report enough surface so attribute access doesn't break.
_git = ("CPython", "", "")
thread_info = type("thread_info", (), {
    "name": "pthread",
    "lock": "mutex+cond",
    "version": None,
})()
def _clear_type_cache(): pass
def _getframe(depth=0):
    # Best-effort: return None for frames pystro doesn't track.
    return None
def audit(*args, **kwargs): pass
def addaudithook(hook): pass
def is_finalizing(): return False

__all__ = ["argv", "path", "version", "version_info", "platform",
           "maxsize", "maxunicode", "byteorder", "prefix", "exec_prefix",
           "executable", "implementation_name",
           "stdin", "stdout", "stderr", "modules",
           "exit", "exc_info", "getrecursionlimit", "setrecursionlimit",
           "getsizeof", "getrefcount", "intern", "settrace", "gettrace",
           "displayhook", "excepthook",
           "__excepthook__", "__displayhook__", "__breakpointhook__",
           "__unraisablehook__",
           "meta_path", "path_hooks", "path_importer_cache"]
