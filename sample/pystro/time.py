# pystro stdlib `time` (minimal).

def time():
    return __pystro_time__()

def sleep(seconds):
    return __pystro_sleep__(seconds)

def perf_counter():
    return __pystro_perf_counter__()

def monotonic():
    return __pystro_perf_counter__()

__all__ = ["time", "sleep", "perf_counter", "monotonic"]
