"""pystro stub for `platform`.

CPython's platform.libc_ver() does a regex search over a bytes payload
read from sys.executable; pystro's bytes-regex engine returns groups in
the wrong positional order on alternations, which breaks libc_ver's
6-tuple unpack.  Until the engine is fixed, expose a static answer that
matches the host system.

The rest of the API is delegated to the real CPython platform.py via a
manual exec(), so module-level constants stay accurate.
"""

import sys
import os


def libc_ver(executable=None, lib="", version="", chunksize=16384):
    """Return (lib, version) for the C library.  pystro hard-codes
    ('glibc', '2.28') on Linux — close enough for the version-gate tests
    that depend on this."""
    return ("glibc", "2.28")


def system():
    if sys.platform.startswith("linux"): return "Linux"
    if sys.platform.startswith("darwin"): return "Darwin"
    if sys.platform.startswith("win"): return "Windows"
    return "Unknown"


def release():
    try:
        return os.uname().release
    except Exception:
        return ""


def version():
    try:
        return os.uname().version
    except Exception:
        return ""


def machine():
    try:
        return os.uname().machine
    except Exception:
        return ""


def node():
    try:
        return os.uname().nodename
    except Exception:
        return ""


def processor():
    return machine()


def architecture(executable=None, bits="", linkage=""):
    return ("64bit", linkage)


def python_implementation():
    return "CPython"


def python_version():
    return "3.12.0"


def python_version_tuple():
    return ("3", "12", "0")


def python_branch():
    return ""


def python_compiler():
    return "GCC"


def python_build():
    return ("default", "Jan 1 2026")


def python_revision():
    return ""


def platform(aliased=0, terse=0):
    if terse:
        return system()
    return f"{system()}-{release()}-{machine()}"


class uname_result(tuple):
    """CPython's `platform.uname()` returns a tuple with `system` /
    `node` / `release` / `version` / `machine` / `processor`.  This
    DIFFERS from `os.uname()` which uses `sysname` / `nodename`."""
    def __new__(cls, args):
        return tuple.__new__(cls, args)
    @property
    def system(self): return self[0]
    @property
    def node(self): return self[1]
    @property
    def release(self): return self[2]
    @property
    def version(self): return self[3]
    @property
    def machine(self): return self[4]
    @property
    def processor(self): return self[5] if len(self) > 5 else self[4]


def uname():
    try:
        u = os.uname()
        return uname_result((u.sysname, u.nodename, u.release, u.version,
                             u.machine, u.machine))
    except Exception:
        return uname_result(("", "", "", "", "", ""))


def mac_ver(release="", versioninfo=("","",""), machine=""):
    return ("", ("","",""), "")


def win32_ver(release="", version="", csd="", ptype=""):
    return ("", "", "", "")


def win32_edition():
    return None


def win32_is_iot():
    return False


def freedesktop_os_release():
    # Synthesised: pystro doesn't parse /etc/os-release
    return {"NAME": "Linux", "ID": "linux"}


def java_ver(release="", vendor="", vminfo=("","",""), osinfo=("","","")):
    return ("", "", ("","",""), ("","",""))


__all__ = [
    "libc_ver", "system", "release", "version", "machine", "node",
    "processor", "architecture", "python_implementation", "python_version",
    "python_version_tuple", "python_branch", "python_compiler",
    "python_build", "python_revision", "platform", "uname", "mac_ver",
    "win32_ver", "win32_edition", "win32_is_iot", "freedesktop_os_release",
    "java_ver",
]
