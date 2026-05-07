# pystro stdlib `os` (minimal).

def getcwd():
    return __pystro_getcwd__()

def getenv(name, default=None):
    return __pystro_getenv__(name, default)

def listdir(path="."):
    return __pystro_listdir__(path)

def remove(path):
    return __pystro_remove__(path)

def makedirs(path, exist_ok=False):
    return __pystro_makedirs__(path, exist_ok)


def mkdir(path, mode=0o777):
    # Pystro's makedirs creates parents as well; mkdir simulates non-existing
    # parent error (mostly).  For our purposes, just call makedirs.
    return __pystro_makedirs__(path, False)


def close(fd):
    # Stub: pystro file objects close themselves; close() of a raw fd is a no-op.
    return None


def unlink(path):
    return __pystro_remove__(path)


def rmdir(path):
    return __pystro_remove__(path)


def fdopen(fd, mode="r"):
    # Stub: not actually wrapping the fd; returns None for now.
    return None


def stat(path):
    # Minimal: return a tuple-like structure with size/mtime.
    return None

# A simple mapping wrapper.
class _Environ:
    def __getitem__(self, k):
        v = __pystro_getenv__(k)
        if v is None:
            raise KeyError(k)
        return v
    def get(self, k, default=None):
        v = __pystro_getenv__(k, default)
        return v

environ = _Environ()


# os.path submodule.
class _Path:
    @staticmethod
    def exists(p):
        return __pystro_path_exists__(p)

    @staticmethod
    def isdir(p):
        return __pystro_isdir__(p)

    @staticmethod
    def isfile(p):
        return __pystro_isfile__(p)

    @staticmethod
    def abspath(p):
        return __pystro_abspath__(p)

    @staticmethod
    def isabs(p):
        return len(p) > 0 and p[0] == "/"

    @staticmethod
    def split(p):
        i = len(p) - 1
        while i >= 0 and p[i] != "/":
            i -= 1
        if i < 0:
            return ("", p)
        if i == 0:
            return ("/", p[1:])
        return (p[:i], p[i+1:])

    @staticmethod
    def join(*parts):
        if not parts:
            return ""
        result = parts[0]
        for p in parts[1:]:
            if not p:
                continue
            if p[0] == "/":
                result = p
            elif result and result[-1] != "/":
                result = result + "/" + p
            else:
                result = result + p
        return result

    @staticmethod
    def basename(p):
        i = len(p) - 1
        while i >= 0 and p[i] != "/":
            i -= 1
        return p[i+1:]

    @staticmethod
    def dirname(p):
        i = len(p) - 1
        while i >= 0 and p[i] != "/":
            i -= 1
        if i < 0:
            return ""
        if i == 0:
            return "/"
        return p[:i]

    @staticmethod
    def expanduser(p):
        if not p or p[0] != "~": return p
        # Find first separator after ~
        i = 1
        while i < len(p) and p[i] != "/":
            i += 1
        if i == 1:
            home = __pystro_getenv__("HOME", "/")
        else:
            # ~user — fall back to /home/user/
            home = "/home/" + p[1:i]
        return home + p[i:]

    @staticmethod
    def expandvars(p):
        # Simple $VAR / ${VAR} substitution.
        out = []
        i = 0
        while i < len(p):
            ch = p[i]
            if ch == "$" and i + 1 < len(p):
                if p[i + 1] == "{":
                    j = p.find("}", i + 2)
                    if j < 0:
                        out.append(ch); i += 1; continue
                    name = p[i + 2:j]
                    val = __pystro_getenv__(name, None)
                    out.append(val if val is not None else "${" + name + "}")
                    i = j + 1
                else:
                    j = i + 1
                    while j < len(p) and (p[j].isalnum() or p[j] == "_"):
                        j += 1
                    if j == i + 1:
                        out.append(ch); i += 1; continue
                    name = p[i + 1:j]
                    val = __pystro_getenv__(name, None)
                    out.append(val if val is not None else "$" + name)
                    i = j
            else:
                out.append(ch); i += 1
        return "".join(out)

    @staticmethod
    def normpath(p):
        if not p: return "."
        # Split on '/'.
        parts = p.split("/")
        absolute = (parts[0] == "")  # leading '/'
        out = []
        for part in parts:
            if part == "" or part == ".":
                continue
            if part == "..":
                if out and out[-1] != "..":
                    out.pop()
                elif not absolute:
                    out.append("..")
            else:
                out.append(part)
        result = "/".join(out)
        if absolute:
            result = "/" + result
        return result or "."

    @staticmethod
    def splitext(p):
        i = len(p) - 1
        while i >= 0 and p[i] != "." and p[i] != "/":
            i -= 1
        if i < 0 or p[i] != "." or i == 0 or p[i-1] == "/":
            return (p, "")
        return (p[:i], p[i:])

    @staticmethod
    def normcase(p):
        # POSIX: identity (case-sensitive).
        return p

    @staticmethod
    def split(p):
        slash = p.rfind("/")
        if slash < 0: return ("", p)
        if slash == 0: return ("/", p[1:])
        return (p[:slash], p[slash+1:])

    @staticmethod
    def commonpath(paths):
        if not paths: return ""
        parts_list = [p.split("/") for p in paths]
        out = []
        for parts in zip(*parts_list):
            if all(p == parts[0] for p in parts):
                out.append(parts[0])
            else:
                break
        return "/".join(out)

    @staticmethod
    def commonprefix(paths):
        if not paths: return ""
        s1 = min(paths)
        s2 = max(paths)
        for i, c in enumerate(s1):
            if c != s2[i]:
                return s1[:i]
        return s1

    @staticmethod
    def relpath(path, start="."):
        # Simple: strip common prefix.
        path_parts = _split_path(_Path.normpath(path))
        start_parts = _split_path(_Path.normpath(start))
        i = 0
        while i < len(path_parts) and i < len(start_parts) and path_parts[i] == start_parts[i]:
            i += 1
        rel = [".."] * (len(start_parts) - i) + path_parts[i:]
        if not rel: return "."
        return "/".join(rel)

    @staticmethod
    def realpath(path):
        return _Path.abspath(path)

    @staticmethod
    def abspath(path):
        if path.startswith("/"): return _Path.normpath(path)
        return _Path.normpath(__pystro_getcwd__() + "/" + path)


def _split_path(p):
    if not p: return []
    parts = p.split("/")
    return [x for x in parts if x and x != "."]

path = _Path()

# Path/separator constants.
sep = "/"
altsep = None
extsep = "."
pathsep = ":"
linesep = "\n"
curdir = "."
pardir = ".."
defpath = ":/bin:/usr/bin"
devnull = "/dev/null"
name = "posix"

def _get_exports_list(module):
    """Mimic the dunder used by `posix` re-export: return module.__all__
    if defined, else dir() filtered to public names."""
    if hasattr(module, "__all__"):
        return module.__all__
    return [n for n in dir(module) if not n.startswith("_")]


__all__ = ["getcwd", "getenv", "environ", "path",
           "listdir", "remove", "unlink", "rmdir", "makedirs",
           "close", "fdopen", "stat",
           "sep", "altsep", "extsep", "pathsep", "linesep",
           "curdir", "pardir", "defpath", "devnull", "name",
           "_get_exports_list"]
