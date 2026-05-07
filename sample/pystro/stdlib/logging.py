# pystro stdlib `logging` — minimal print-based stub.
import sys

DEBUG = 10
INFO = 20
WARNING = 30
ERROR = 40
CRITICAL = 50

_level = WARNING
_format = "%(levelname)s:%(name)s:%(message)s"


def basicConfig(**kwargs):
    global _level, _format
    if "level" in kwargs: _level = kwargs["level"]
    if "format" in kwargs: _format = kwargs["format"]


class Logger:
    def __init__(self, name="root"):
        self.name = name
        self.level = _level
    def setLevel(self, level):
        self.level = level
    def isEnabledFor(self, level):
        return level >= self.level
    def _log(self, levelname, level, msg, args):
        if level < self.level:
            return
        if args:
            try:
                msg = msg % args
            except Exception:
                pass
        print(levelname + ":" + self.name + ":" + str(msg))
    def debug(self, msg, *args):     self._log("DEBUG", DEBUG, msg, args)
    def info(self, msg, *args):      self._log("INFO", INFO, msg, args)
    def warning(self, msg, *args):   self._log("WARNING", WARNING, msg, args)
    def warn(self, msg, *args):      self._log("WARNING", WARNING, msg, args)
    def error(self, msg, *args):     self._log("ERROR", ERROR, msg, args)
    def critical(self, msg, *args):  self._log("CRITICAL", CRITICAL, msg, args)
    def exception(self, msg, *args): self._log("ERROR", ERROR, msg, args)


_root = Logger("root")


def getLogger(name="root"):
    return Logger(name) if name != "root" else _root


def debug(msg, *args):     _root.debug(msg, *args)
def info(msg, *args):      _root.info(msg, *args)
def warning(msg, *args):   _root.warning(msg, *args)
def error(msg, *args):     _root.error(msg, *args)
def critical(msg, *args):  _root.critical(msg, *args)
