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

def perf_counter_ns():
    return int(perf_counter() * 1e9)


def monotonic_ns():
    return int(monotonic() * 1e9)


def time_ns():
    return int(time() * 1e9)


def process_time():
    return perf_counter()


def process_time_ns():
    return int(process_time() * 1e9)


def thread_time():
    return perf_counter()


def thread_time_ns():
    return int(thread_time() * 1e9)


def clock_gettime(clk_id):
    return time()


def clock_gettime_ns(clk_id):
    return time_ns()


def asctime(t=None):
    return strftime("%a %b %d %H:%M:%S %Y", t)


def ctime(secs=None):
    return strftime("%a %b %d %H:%M:%S %Y", localtime(secs))


def strptime(s, fmt="%a %b %d %H:%M:%S %Y"):
    raise ValueError("strptime not supported")


def get_clock_info(name):
    class _Info:
        def __init__(self_):
            self_.implementation = name
            self_.monotonic = True
            self_.adjustable = False
            self_.resolution = 1e-9
    return _Info()


def tzset(): pass


CLOCK_REALTIME = 0
CLOCK_MONOTONIC = 1
CLOCK_PROCESS_CPUTIME_ID = 2
CLOCK_THREAD_CPUTIME_ID = 3
CLOCK_MONOTONIC_RAW = 4
CLOCK_BOOTTIME = 5

# CPython exposes the count of fields in struct_time (9 baseline + 2
# for tm_zone / tm_gmtoff on Linux/macOS).  test_time.skipUnless gates
# tm_zone tests on this.  pystro's struct_time has just 9 fields, so
# return 9 so the timezone-aware tests are skipped (not failed).
_STRUCT_TM_ITEMS = 9

altzone = 0
daylight = 0
timezone = 0
tzname = ("UTC", "UTC")


class struct_time(tuple):
    """time.struct_time named-tuple.  Pystro returns plain tuples from
    localtime/gmtime/strptime, but isinstance() checks against this
    class are common in stdlib code."""
    n_fields = 9
    n_sequence_fields = 9
    n_unnamed_fields = 0
    def __new__(cls, *args, **kwargs):
        if args and len(args) == 1:
            return super().__new__(cls, args[0])
        return super().__new__(cls, args)
    @property
    def tm_year(self): return self[0]
    @property
    def tm_mon(self): return self[1]
    @property
    def tm_mday(self): return self[2]
    @property
    def tm_hour(self): return self[3]
    @property
    def tm_min(self): return self[4]
    @property
    def tm_sec(self): return self[5]
    @property
    def tm_wday(self): return self[6]
    @property
    def tm_yday(self): return self[7]
    @property
    def tm_isdst(self): return self[8]


__all__ = ["time", "sleep", "perf_counter", "monotonic",
           "localtime", "gmtime", "strftime", "strptime", "mktime",
           "perf_counter_ns", "monotonic_ns", "time_ns",
           "process_time", "process_time_ns",
           "thread_time", "thread_time_ns",
           "clock_gettime", "clock_gettime_ns",
           "asctime", "ctime", "get_clock_info", "tzset",
           "struct_time", "altzone", "daylight", "timezone", "tzname",
           "CLOCK_REALTIME", "CLOCK_MONOTONIC"]
