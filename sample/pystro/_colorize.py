"""pystro stub for `_colorize` (CPython 3.13+ traceback colorization).
The CPython implementation uses many dataclasses with intricate
inheritance (Mapping[str,str]) that depend on CPython-internal class-
construction details pystro doesn't fully replicate.  This stub
provides flat no-op classes/objects that satisfy the imports."""

COLORIZE = False


def can_colorize():
    return False


def get_theme(force_color=False, force_no_color=False):
    return _Theme()


def get_colors(*args, **kwargs):
    return _Theme()


def colorize_disabled(*a, **kw):
    return False


class ANSIColors:
    RESET = ""
    BLACK = ""; RED = ""; GREEN = ""; YELLOW = ""; BLUE = ""
    MAGENTA = ""; CYAN = ""; WHITE = ""
    GREY = ""
    BOLD = ""
    BOLD_BLACK = ""
    BOLD_RED = ""; BOLD_GREEN = ""; BOLD_YELLOW = ""; BOLD_BLUE = ""
    BOLD_MAGENTA = ""; BOLD_CYAN = ""; BOLD_WHITE = ""


class CursesColors:
    BLACK = 0; RED = 1; GREEN = 2; YELLOW = 3; BLUE = 4
    MAGENTA = 5; CYAN = 6; WHITE = 7
    DEFAULT = -1
    BG_BLACK = 0; BG_RED = 1; BG_GREEN = 2; BG_YELLOW = 3
    BG_BLUE = 4; BG_MAGENTA = 5; BG_CYAN = 6; BG_WHITE = 7
    BG_DEFAULT = -1
    NORMAL = 0
    REVERSE = 1


class _ThemeSection:
    def __init__(self, **kwargs):
        for k, v in kwargs.items():
            setattr(self, k, v)
    def __getitem__(self, key):
        return getattr(self, key, "")
    def __getattr__(self, name):
        return ""
    def __contains__(self, key): return True
    def __iter__(self): return iter([])
    def __len__(self): return 0
    @classmethod
    def no_colors(cls): return cls()
    def copy_with(self, **kwargs): return self


class Argparse(_ThemeSection): pass
class Ast(_ThemeSection): pass
class Calendar(_ThemeSection): pass
class Difflib(_ThemeSection): pass
class FancyCompleter(_ThemeSection): pass
class HttpServer(_ThemeSection): pass
class LiveProfiler(_ThemeSection): pass
class Pdb(_ThemeSection): pass
class Pyrepl(_ThemeSection): pass
class Syntax(_ThemeSection): pass
class Traceback(_ThemeSection): pass
class Unittest(_ThemeSection): pass


class _Theme:
    def __init__(self, **kw):
        self.argparse = Argparse()
        self.ast = Ast()
        self.calendar = Calendar()
        self.difflib = Difflib()
        self.fancy_completer = FancyCompleter()
        self.http_server = HttpServer()
        self.live_profiler = LiveProfiler()
        self.pdb = Pdb()
        self.pyrepl = Pyrepl()
        self.syntax = Syntax()
        self.traceback = Traceback()
        self.unittest = Unittest()
    def copy_with(self, **kw):
        return self
    @classmethod
    def no_colors(cls):
        return cls()


Theme = _Theme
default_theme = _Theme()
theme_no_color = _Theme()


def set_theme(theme):
    pass


__all__ = ["COLORIZE", "can_colorize", "get_theme", "get_colors",
           "ANSIColors", "CursesColors",
           "Argparse", "Ast", "Calendar", "Difflib", "FancyCompleter",
           "HttpServer", "LiveProfiler", "Pdb", "Pyrepl", "Syntax",
           "Traceback", "Unittest", "Theme", "default_theme",
           "theme_no_color", "set_theme"]
