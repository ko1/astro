# pystro stdlib `sys` (minimal).

argv = __pystro_argv__()

# `sys.path` — module search path.
path = ["", __pystro_getcwd__()]

# Version info.
version = "pystro 0.1"
version_info = (0, 1, 0)
hexversion = 0x000100F0
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

modules = {}

def exit(code=0):
    return __pystro_exit__(code)

def getrecursionlimit():
    return 1000

def setrecursionlimit(n):
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
           "exit", "getrecursionlimit", "setrecursionlimit",
           "getsizeof", "getrefcount", "intern", "settrace", "gettrace",
           "displayhook", "excepthook"]
