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
    def splitext(p):
        i = len(p) - 1
        while i >= 0 and p[i] != "." and p[i] != "/":
            i -= 1
        if i < 0 or p[i] != "." or i == 0 or p[i-1] == "/":
            return (p, "")
        return (p[:i], p[i:])

path = _Path()

__all__ = ["getcwd", "getenv", "environ", "path",
           "listdir", "remove", "makedirs"]
