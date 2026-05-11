"""Stub for test.support.script_helper."""
import unittest


def assert_python_ok(*args, **kwargs):
    raise unittest.SkipTest("subprocess not supported")


def assert_python_failure(*args, **kwargs):
    raise unittest.SkipTest("subprocess not supported")


def run_python_until_end(*args, **kwargs):
    raise unittest.SkipTest("subprocess not supported")


def spawn_python(*args, **kwargs):
    raise unittest.SkipTest("subprocess not supported")


def kill_python(p): pass


def make_script(script_dir, script_basename, source, omit_suffix=False):
    return script_dir + "/" + script_basename


def make_zip_script(*a, **kw):
    raise unittest.SkipTest("zipfile not supported")


def interpreter_requires_environment():
    return False


def run_test_script(*args, **kwargs):
    """Stub: pystro can't shell out to a child interpreter."""
    raise unittest.SkipTest("subprocess not supported")


def _assert_python(expected_success, /, *args, **env_vars):
    """Internal helper used by assert_python_*; pystro has no
    subprocess, so any caller observing this should also be SkipTest'd."""
    raise unittest.SkipTest("subprocess not supported")


def make_pkg(pkg_dir, *modules, init_source=""):
    """Create a Python package on disk."""
    import os
    os.makedirs(pkg_dir, exist_ok=True)
    with open(os.path.join(pkg_dir, "__init__.py"), "w") as f:
        f.write(init_source)
    return pkg_dir


def make_zip_pkg(*args, **kwargs):
    raise unittest.SkipTest("zipfile not supported")


__all__ = ["assert_python_ok", "assert_python_failure", "run_python_until_end",
           "spawn_python", "kill_python", "make_script", "make_zip_script",
           "interpreter_requires_environment", "run_test_script"]
