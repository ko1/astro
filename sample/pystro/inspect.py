# pystro stdlib `inspect` (minimal).

def isfunction(o):
    return type(o).__name__ == "function"

def isbuiltin(o):
    return type(o).__name__ == "builtin_function_or_method"

def ismethod(o):
    return type(o).__name__ == "method"

def isclass(o):
    return isinstance(o, type)

def ismodule(o):
    return type(o).__name__ == "module"

def isgenerator(o):
    return type(o).__name__ == "generator"

def isgeneratorfunction(fn):
    # Pystro doesn't expose a flag; conservative default.
    return False

def iscoroutinefunction(fn):
    return False

def isawaitable(o):
    return False

def isabstract(cls):
    return False


def getmembers(obj, predicate=None):
    out = []
    for name in dir(obj):
        try:
            v = getattr(obj, name)
        except Exception:
            continue
        if predicate is None or predicate(v):
            out.append((name, v))
    out.sort()
    return out


def getmro(cls):
    return cls.__mro__ if hasattr(cls, "__mro__") else (cls,)


def signature(fn):
    return _Signature(fn)


class Parameter:
    POSITIONAL_ONLY = 0
    POSITIONAL_OR_KEYWORD = 1
    VAR_POSITIONAL = 2
    KEYWORD_ONLY = 3
    VAR_KEYWORD = 4
    empty = object()
    def __init__(self, name, kind=POSITIONAL_OR_KEYWORD, default=None, annotation=None):
        self.name = name
        self.kind = kind
        self.default = default
        self.annotation = annotation


class _Signature:
    def __init__(self, fn):
        self.fn = fn
        self.parameters = {}
        self.return_annotation = None
        # Build best-effort parameter list from __defaults__ /
        # __kwdefaults__.  Names are not directly exposed, so we use
        # numeric placeholders ("arg0", "arg1", ...) when unknown.
        kw = {}
        try:
            kw = fn.__kwdefaults__ or {}
        except (AttributeError, TypeError):
            kw = {}
        self._kw = kw
        defaults = ()
        try:
            defaults = fn.__defaults__ or ()
        except (AttributeError, TypeError):
            defaults = ()
        self._defaults = defaults
    def __str__(self):
        # Render '(a, b=2, *, c=3)' style.  Names not introspectable, so
        # we omit names for positional and just show defaults.
        parts = []
        for i, d in enumerate(self._defaults):
            parts.append("arg" + str(i) + "=" + repr(d))
        for k, v in self._kw.items():
            parts.append(str(k) + "=" + repr(v))
        return "(" + ", ".join(parts) + ")"
    def __repr__(self):
        return "<Signature " + str(self) + ">"


def getsource(o):
    return ""

def getsourcelines(o):
    return ([], 0)

def getfile(o):
    return "<unknown>"

def stack():
    return []

def currentframe():
    return None

def getouterframes(frame, context=1):
    return []

def getinnerframes(traceback, context=1):
    return []
