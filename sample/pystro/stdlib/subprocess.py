"""pystro stub for `subprocess`.  pystro can't fork/exec, so this
just provides API placeholders that raise on use."""


PIPE = -1
STDOUT = -2
DEVNULL = -3


class SubprocessError(Exception): pass
class CalledProcessError(SubprocessError):
    def __init__(self, returncode, cmd, output=None, stderr=None):
        self.returncode = returncode
        self.cmd = cmd
        self.output = output
        self.stderr = stderr
class TimeoutExpired(SubprocessError):
    def __init__(self, cmd, timeout, output=None, stderr=None):
        self.cmd = cmd; self.timeout = timeout
        self.output = output; self.stderr = stderr


class Popen:
    def __init__(self, *args, **kwargs):
        raise SubprocessError("subprocess not supported in pystro")
    def __enter__(self): return self
    def __exit__(self, *e): return False


def run(*args, **kwargs):
    raise SubprocessError("subprocess not supported")


def call(*args, **kwargs):
    raise SubprocessError("subprocess not supported")


def check_call(*args, **kwargs):
    raise SubprocessError("subprocess not supported")


def check_output(*args, **kwargs):
    raise SubprocessError("subprocess not supported")


def getoutput(cmd):
    return ""


def getstatusoutput(cmd):
    return (1, "")


class CompletedProcess:
    def __init__(self, args, returncode, stdout=None, stderr=None):
        self.args = args; self.returncode = returncode
        self.stdout = stdout; self.stderr = stderr


__all__ = ["Popen", "PIPE", "STDOUT", "DEVNULL",
           "SubprocessError", "CalledProcessError", "TimeoutExpired",
           "run", "call", "check_call", "check_output",
           "getoutput", "getstatusoutput", "CompletedProcess"]
