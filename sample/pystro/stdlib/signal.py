# pystro stub for `signal` (the Python wrapper around _signal).
from _signal import *
from _signal import (SIGINT, SIGTERM, SIG_DFL, SIG_IGN, NSIG,
                     signal, getsignal, default_int_handler,
                     valid_signals)


class Signals:
    SIGINT = SIGINT
    SIGTERM = SIGTERM


def Handlers(*a, **kw): return None


__all__ = ["SIGINT", "SIGTERM", "SIGHUP", "SIGQUIT", "SIGKILL",
           "SIG_DFL", "SIG_IGN", "NSIG",
           "signal", "getsignal", "default_int_handler",
           "valid_signals", "Signals"]
