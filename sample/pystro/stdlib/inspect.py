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
        # Try to extract param names from __code__.
        names = ()
        argc = 0
        try:
            code = fn.__code__
            names = tuple(code.co_varnames) if code else ()
            argc = code.co_argcount if code else 0
        except (AttributeError, TypeError):
            pass
        self._names = names
        self._argc = argc
        anns = {}
        try:
            anns = fn.__annotations__ or {}
        except (AttributeError, TypeError):
            anns = {}
        self.return_annotation = anns.get("return", Parameter.empty)
        params = {}
        n_def = len(defaults)
        for i, n in enumerate(names[:argc]):
            d = defaults[i - (argc - n_def)] if i >= argc - n_def else Parameter.empty
            ann = anns.get(n, Parameter.empty)
            params[n] = Parameter(n, Parameter.POSITIONAL_OR_KEYWORD, d, ann)
        for n in names[argc:]:
            d = kw.get(n, Parameter.empty)
            ann = anns.get(n, Parameter.empty)
            params[n] = Parameter(n, Parameter.KEYWORD_ONLY, d, ann)
        self.parameters = params
    def __str__(self):
        n_def = len(self._defaults)
        parts = []
        for i, n in enumerate(self._names[:self._argc]):
            if i >= self._argc - n_def:
                d = self._defaults[i - (self._argc - n_def)]
                parts.append(n + "=" + repr(d))
            else:
                parts.append(n)
        if self._argc < len(self._names):
            parts.append("*")
            for n in self._names[self._argc:]:
                if n in self._kw:
                    parts.append(n + "=" + repr(self._kw[n]))
                else:
                    parts.append(n)
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
