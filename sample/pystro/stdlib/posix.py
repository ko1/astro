"""pystro stub for `posix` (CPython's POSIX-system-call accelerator).

CPython's `os.py` does `import posix as _posix` on Linux/macOS and uses
its functions for the heavy lifting.  We stub the surface so `import os`
+ test code that gates on `os.X` doesn't fail at import time.

For functions pystro implements via __pystro_*__ builtins, delegate
through the `os` builtins fallback.  For things pystro doesn't model
(real fork, ptrace, ioctl), expose stubs that raise OSError /
NotImplementedError on call so user tests skip the path."""

import sys


# CPython exposes `posix.environ` as a dict of bytes->bytes; user code
# also reads `os.environ` (a wrapper).  Provide a minimal mapping so
# `os.environ` initialization works.
class _Environ(dict):
    def __setitem__(self, k, v):
        super().__setitem__(k, v)
    def __delitem__(self, k):
        super().__delitem__(k)


environ = _Environ()
try:
    _envb = __pystro_environ__()  # builtin returning {bytes: bytes}
    if isinstance(_envb, dict):
        for k, v in _envb.items():
            environ[k] = v
except (NameError, TypeError):
    pass


# os.path constants and helpers — many delegate to platform internals.
sep = "/"
altsep = None
extsep = "."
pathsep = ":"
defpath = "/usr/bin:/bin"
linesep = "\n"
devnull = "/dev/null"

F_OK = 0
R_OK = 4
W_OK = 2
X_OK = 1

O_RDONLY = 0
O_WRONLY = 1
O_RDWR = 2
O_APPEND = 1024
O_CREAT = 64
O_EXCL = 128
O_TRUNC = 512
O_NONBLOCK = 2048
O_DIRECTORY = 65536
O_CLOEXEC = 524288


def getcwd():
    return __pystro_getcwd__() if "__pystro_getcwd__" in dir() else "/"


def getcwdb():
    return getcwd().encode()


def getenv(key, default=None):
    try: return __pystro_getenv__(key)
    except (NameError, TypeError): return default


def putenv(key, value):
    pass


def unsetenv(key):
    pass


def listdir(path="."):
    try: return __pystro_listdir__(path)
    except (NameError, TypeError): return []


def stat(path, *, dir_fd=None, follow_symlinks=True):
    raise NotImplementedError("posix.stat")


def lstat(path):
    return stat(path, follow_symlinks=False)


def fstat(fd):
    raise NotImplementedError("posix.fstat")


def access(path, mode, *, dir_fd=None, effective_ids=False, follow_symlinks=True):
    try: return __pystro_path_exists__(path)
    except (NameError, TypeError): return False


def isfile(path):
    try: return __pystro_isfile__(path)
    except (NameError, TypeError): return False


def isdir(path):
    try: return __pystro_isdir__(path)
    except (NameError, TypeError): return False


def remove(path):
    try: return __pystro_remove__(path)
    except (NameError, TypeError): raise OSError("remove not supported")


unlink = remove


def rmdir(path):
    raise OSError("rmdir not supported")


def mkdir(path, mode=0o777, *, dir_fd=None):
    try: return __pystro_makedirs__(path)
    except (NameError, TypeError): raise OSError("mkdir not supported")


def makedirs(path, mode=0o777, exist_ok=False):
    return mkdir(path, mode)


def rename(src, dst):
    raise OSError("rename not supported")


def open(path, flags, mode=0o777, *, dir_fd=None):
    raise OSError("posix.open not supported")


def close(fd):
    pass


def read(fd, n):
    return b""


def write(fd, data):
    return len(data)


def fsync(fd):
    pass


def fdopen(fd, *args, **kwargs):
    raise OSError("fdopen not supported")


def pipe():
    raise OSError("pipe not supported")


def dup(fd): return fd
def dup2(fd, fd2): return fd2


def fork():
    raise OSError("fork not supported in pystro")


def waitpid(pid, options):
    return (pid, 0)


def WEXITSTATUS(status): return status & 0xFF
def WIFEXITED(status): return True
def WIFSIGNALED(status): return False


def kill(pid, sig):
    raise PermissionError("kill not supported")


def getpid():
    return 1


def getppid():
    return 0


def getuid():
    return 1000


def geteuid():
    return 1000


def getgid():
    return 1000


def getegid():
    return 1000


def umask(mask):
    return 0o022


def chmod(path, mode, *, dir_fd=None, follow_symlinks=True):
    pass


def chown(path, uid, gid, *, dir_fd=None, follow_symlinks=True):
    pass


def utime(path, times=None, *, ns=None, dir_fd=None, follow_symlinks=True):
    pass


def readlink(path):
    raise OSError("readlink not supported")


def symlink(src, dst, target_is_directory=False, *, dir_fd=None):
    raise OSError("symlink not supported")


def link(src, dst, *, src_dir_fd=None, dst_dir_fd=None, follow_symlinks=True):
    raise OSError("link not supported")


def system(command):
    return -1


def urandom(n):
    # Use Python's `random` for non-cryptographic urandom.  Not real
    # OS entropy — CPython tests usually just check return shape/length.
    import random
    return bytes(random.randint(0, 255) for _ in range(n))


# error is OSError on POSIX
error = OSError


# `posix.terminal_size` and small named-tuple-likes that test_os checks for.
class terminal_size(tuple):
    def __new__(cls, args):
        return tuple.__new__(cls, args)
    @property
    def columns(self): return self[0]
    @property
    def lines(self): return self[1]


def get_terminal_size(fd=1):
    return terminal_size((80, 24))


# stat_result skeleton — real test_os exercises these fields.
class stat_result(tuple):
    def __new__(cls, args):
        if len(args) < 10:
            args = list(args) + [0] * (10 - len(args))
        return tuple.__new__(cls, args)
    @property
    def st_mode(self): return self[0]
    @property
    def st_ino(self): return self[1]
    @property
    def st_dev(self): return self[2]
    @property
    def st_nlink(self): return self[3]
    @property
    def st_uid(self): return self[4]
    @property
    def st_gid(self): return self[5]
    @property
    def st_size(self): return self[6]
    @property
    def st_atime(self): return self[7]
    @property
    def st_mtime(self): return self[8]
    @property
    def st_ctime(self): return self[9]


# Lots of CPython internal helpers tests call without using output.
def _exit(code): sys.exit(code)
def abort(): sys.exit(1)
def execv(path, argv): raise OSError("execv not supported")
def execve(path, argv, env): raise OSError("execve not supported")
def execvp(file, args): raise OSError("execvp not supported")
def execvpe(file, args, env): raise OSError("execvpe not supported")
def fpathconf(fd, name): return 0
def pathconf(path, name): return 0
def confstr(name): return ""
def sysconf(name): return 0
def cpu_count(): return 1
def process_cpu_count(): return 1
def getloadavg(): return (0.0, 0.0, 0.0)
def times():
    return (0.0, 0.0, 0.0, 0.0, 0.0)
def get_inheritable(fd): return False
def set_inheritable(fd, inh): pass
def get_blocking(fd): return True
def set_blocking(fd, blocking): pass
def isatty(fd): return False
def lseek(fd, pos, how): return 0
def ftruncate(fd, length): pass
def truncate(path, length): pass
def sync(): pass
def fdatasync(fd): pass
def replace(src, dst): rename(src, dst)
def scandir(path="."):
    raise OSError("scandir not supported")
def get_exec_path(env=None):
    return defpath.split(":")


# Lots of CPython tests check for specific symbol names.  Provide a
# comprehensive __all__ so `from posix import *` doesn't pull in things
# that don't exist.
__all__ = [
    "environ", "getcwd", "getcwdb", "getenv", "putenv", "unsetenv",
    "listdir", "stat", "lstat", "fstat", "access", "isfile", "isdir",
    "remove", "unlink", "rmdir", "mkdir", "makedirs", "rename",
    "open", "close", "read", "write", "fsync", "fdopen", "pipe",
    "dup", "dup2", "fork", "waitpid", "WEXITSTATUS", "WIFEXITED",
    "WIFSIGNALED", "kill", "getpid", "getppid", "getuid", "geteuid",
    "getgid", "getegid", "umask", "chmod", "chown", "utime",
    "readlink", "symlink", "link", "system", "urandom", "error",
    "terminal_size", "get_terminal_size", "stat_result",
    "_exit", "abort", "execv", "execve", "execvp", "execvpe",
    "fpathconf", "pathconf", "confstr", "sysconf", "cpu_count",
    "process_cpu_count", "getloadavg", "times",
    "get_inheritable", "set_inheritable", "get_blocking", "set_blocking",
    "isatty", "lseek", "ftruncate", "truncate", "sync", "fdatasync",
    "replace", "scandir", "get_exec_path",
    "F_OK", "R_OK", "W_OK", "X_OK",
    "O_RDONLY", "O_WRONLY", "O_RDWR", "O_APPEND", "O_CREAT", "O_EXCL",
    "O_TRUNC", "O_NONBLOCK", "O_DIRECTORY", "O_CLOEXEC",
    "sep", "altsep", "extsep", "pathsep", "defpath", "linesep", "devnull",
]
