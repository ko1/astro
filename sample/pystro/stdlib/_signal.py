# pystro stub for `_signal` (CPython C extension for signal handling).
# pystro doesn't propagate OS signals; this stub keeps imports working.

SIGINT = 2
SIGTERM = 15
SIGHUP = 1
SIGQUIT = 3
SIGKILL = 9
SIGUSR1 = 10
SIGUSR2 = 12
SIGABRT = 6
SIGALRM = 14
SIGCHLD = 17
SIGPIPE = 13
SIGSTOP = 19
SIGTSTP = 20
SIGCONT = 18
NSIG = 32

SIG_DFL = 0
SIG_IGN = 1


def signal(signum, handler):
    return None


def getsignal(signum):
    return SIG_DFL


def default_int_handler(signum, frame):
    raise KeyboardInterrupt


def alarm(seconds):
    return 0


def pause():
    pass


def raise_signal(sig):
    pass


def set_wakeup_fd(fd):
    return -1


def siginterrupt(signum, flag):
    pass


def pthread_kill(thread_id, signum):
    pass


def valid_signals():
    return frozenset(range(1, NSIG))


def strsignal(signalnum):
    return "Signal " + str(signalnum)


__all__ = ["SIGINT", "SIGTERM", "SIGHUP", "SIGQUIT", "SIGKILL",
           "SIG_DFL", "SIG_IGN", "NSIG",
           "signal", "getsignal", "default_int_handler", "alarm",
           "pause", "raise_signal", "set_wakeup_fd", "siginterrupt",
           "valid_signals", "strsignal"]
