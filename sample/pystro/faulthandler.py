"""pystro stub for `faulthandler`."""
import sys


def enable(file=None, all_threads=True): pass
def disable(): pass
def is_enabled(): return False
def register(signum, file=None, all_threads=True, chain=False): pass
def unregister(signum): pass
def dump_traceback(file=None, all_threads=True): pass
def dump_traceback_later(timeout, repeat=False, file=None, exit=False): pass
def cancel_dump_traceback_later(): pass


__all__ = ["enable", "disable", "is_enabled", "register", "unregister",
           "dump_traceback", "dump_traceback_later",
           "cancel_dump_traceback_later"]
