"""Stub for test.support.socket_helper."""
import unittest


HOST = "127.0.0.1"
HOSTv4 = HOST
HOSTv6 = "::1"


def find_unused_port(family=None, socktype=None):
    raise unittest.SkipTest("socket not supported")


def bind_port(sock, host=HOST):
    raise unittest.SkipTest("socket not supported")


def bind_unix_socket(sock, addr):
    raise unittest.SkipTest("unix socket not supported")


def transient_internet(*a, **kw):
    class _Ctx:
        def __enter__(self): return self
        def __exit__(self, *e): return False
    return _Ctx()


def skip_if_tcp_blackhole(fn):
    return fn


__all__ = ["HOST", "HOSTv4", "HOSTv6",
           "find_unused_port", "bind_port", "bind_unix_socket",
           "transient_internet", "skip_if_tcp_blackhole"]
