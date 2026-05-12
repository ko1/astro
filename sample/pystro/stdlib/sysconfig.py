"""pystro stub for `sysconfig` (CPython build-time config).

Pystro doesn't expose CPython's `_sysconfigdata_*` module that the real
sysconfig depends on.  Provide a minimal API that returns plausible
values so test code that checks `get_config_var` / `get_paths` /
`get_platform` doesn't crash.
"""

import sys
import os


_PREFIX = getattr(sys, "prefix", "/usr/local")
_EXEC_PREFIX = getattr(sys, "exec_prefix", "/usr/local")
_PROJECT_BASE = _PREFIX
_BASE_PREFIX = getattr(sys, "base_prefix", _PREFIX)
_BASE_EXEC_PREFIX = getattr(sys, "base_exec_prefix", _EXEC_PREFIX)
_PY_VERSION = "3.12.0"
_PY_VERSION_SHORT = "3.12"
_PY_VERSION_SHORT_NO_DOT = "312"


_CONFIG_VARS = {
    "prefix":              _PREFIX,
    "exec_prefix":         _EXEC_PREFIX,
    "py_version":          _PY_VERSION,
    "py_version_short":    _PY_VERSION_SHORT,
    "py_version_nodot":    _PY_VERSION_SHORT_NO_DOT,
    "installed_base":      _BASE_PREFIX,
    "base":                _PREFIX,
    "installed_platbase":  _BASE_EXEC_PREFIX,
    "platbase":            _EXEC_PREFIX,
    "projectbase":         _PROJECT_BASE,
    "platlibdir":          getattr(sys, "platlibdir", "lib"),
    "abiflags":            "",
    "py_version_nodot_plat": "",
    "EXT_SUFFIX":          ".so",
    "SHLIB_SUFFIX":        ".so",
    "SOABI":               "pystro-3.12-linux-gnu",
    "SO":                  ".so",
    "MULTIARCH":           "",
    "userbase":            os.path.expanduser("~/.local"),
}


_INSTALL_SCHEMES = {
    "posix_prefix": {
        "stdlib":      "{base}/lib/python{py_version_short}",
        "platstdlib":  "{platbase}/lib/python{py_version_short}",
        "purelib":     "{base}/lib/python{py_version_short}/site-packages",
        "platlib":     "{platbase}/lib/python{py_version_short}/site-packages",
        "include":     "{base}/include/python{py_version_short}",
        "platinclude": "{platbase}/include/python{py_version_short}",
        "scripts":     "{base}/bin",
        "data":        "{base}",
    },
}
_INSTALL_SCHEMES["posix_user"] = _INSTALL_SCHEMES["posix_prefix"]
_INSTALL_SCHEMES["nt"] = _INSTALL_SCHEMES["posix_prefix"]


def get_config_var(name):
    return _CONFIG_VARS.get(name)


def get_config_vars(*args):
    if not args: return dict(_CONFIG_VARS)
    return [_CONFIG_VARS.get(a) for a in args]


def get_paths(scheme=None, vars=None, expand=True):
    s = _INSTALL_SCHEMES.get(scheme or "posix_prefix") or {}
    if not expand: return dict(s)
    out = {}
    cv = dict(_CONFIG_VARS)
    if vars: cv.update(vars)
    for k, v in s.items():
        try:
            out[k] = v.format(**cv)
        except KeyError:
            out[k] = v
    return out


def get_path(name, scheme=None, vars=None, expand=True):
    return get_paths(scheme, vars, expand).get(name, "")


def get_path_names():
    return ("stdlib", "platstdlib", "purelib", "platlib",
            "include", "platinclude", "scripts", "data")


def get_scheme_names():
    return tuple(_INSTALL_SCHEMES)


def get_platform():
    return "linux-x86_64"


def get_python_version():
    return _PY_VERSION_SHORT


def get_makefile_filename():
    return "/dev/null/Makefile"


def get_default_scheme():
    return "posix_prefix"


def get_preferred_scheme(key):
    return "posix_prefix"


def parse_config_h(fp, vars=None):
    return vars or {}


def get_config_h_filename():
    return "/dev/null/pyconfig.h"


def is_python_build(check_home=None):
    return False


def expand_makefile_vars(s, vars):
    return s


def _expand_vars(scheme, vars):
    return get_paths(scheme, vars)


def _get_default_scheme():
    return "posix_prefix"


def _get_preferred_schemes():
    return {
        "prefix": "posix_prefix",
        "home": "posix_home",
        "user": "posix_user",
    }


def get_preferred_scheme(key):
    return _get_preferred_schemes().get(key, "posix_prefix")


_PYTHON_BUILD = False
