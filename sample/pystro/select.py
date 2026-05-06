"""pystro stub for `select` (I/O multiplexing).  pystro is single-
threaded synchronous; these all skip / no-op."""


def select(rlist, wlist, xlist, timeout=None):
    return ([], [], [])


def poll():
    return _Poll()


class _Poll:
    def register(self, fd, eventmask=0): pass
    def unregister(self, fd): pass
    def modify(self, fd, eventmask): pass
    def poll(self, timeout=None): return []


POLLIN = 1
POLLPRI = 2
POLLOUT = 4
POLLERR = 8
POLLHUP = 16
POLLNVAL = 32

PIPE_BUF = 4096


error = OSError


__all__ = ["select", "poll", "POLLIN", "POLLPRI", "POLLOUT",
           "POLLERR", "POLLHUP", "POLLNVAL", "PIPE_BUF", "error"]
