# pystro stdlib `time` (minimal).

def time():
    return __pystro_time__()

def sleep(seconds):
    return __pystro_sleep__(seconds)

def perf_counter():
    return __pystro_perf_counter__()

def monotonic():
    return __pystro_perf_counter__()

def localtime(secs=None):
    if secs is None:
        return __pystro_localtime__()
    return __pystro_localtime__(secs)

def gmtime(secs=None):
    if secs is None:
        return __pystro_gmtime__()
    return __pystro_gmtime__(secs)

def strftime(fmt, t=None):
    if t is None:
        return __pystro_strftime__(fmt)
    return __pystro_strftime__(fmt, t)

def mktime(t):
    # Approximate via attribute access — pystro doesn't have a true mktime.
    return 0.0

__all__ = ["time", "sleep", "perf_counter", "monotonic",
           "localtime", "gmtime", "strftime", "mktime"]
