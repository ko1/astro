# pystro stdlib `pprint` (minimal).  CPython's pprint has many niche
# options; this implementation handles the common cases (dict, list,
# tuple, set, primitive) with optional `indent` / `width`.

def _format(obj, indent, width, level=0):
    """Render obj with single- or multi-line format."""
    single = repr(obj)
    if len(single) + level * indent <= width:
        return single
    if isinstance(obj, dict):
        prefix = " " * (level * indent + indent)
        out_prefix = " " * (level * indent)
        items = []
        for k, v in obj.items():
            items.append(prefix + repr(k) + ": " + _format(v, indent, width, level + 1))
        return "{\n" + ",\n".join(items) + "}\n".rstrip("\n").rstrip() if False else \
               "{" + "\n" + ",\n".join(items) + "\n" + out_prefix + "}"
    if isinstance(obj, (list, tuple, set)):
        prefix = " " * (level * indent + indent)
        out_prefix = " " * (level * indent)
        items = [prefix + _format(x, indent, width, level + 1) for x in obj]
        if isinstance(obj, list):
            open_ch, close_ch = "[", "]"
        elif isinstance(obj, tuple):
            open_ch, close_ch = "(", ")"
        else:
            open_ch, close_ch = "{", "}"
        return open_ch + "\n" + ",\n".join(items) + "\n" + out_prefix + close_ch
    return single


def pformat(obj, indent=1, width=80, depth=None, *, compact=False, sort_dicts=True):
    """Return pretty-printed string."""
    return _format(obj, indent, width, 0)


def pprint(obj, stream=None, indent=1, width=80, depth=None,
           *, compact=False, sort_dicts=True):
    """Print pretty-formatted obj."""
    s = pformat(obj, indent=indent, width=width, depth=depth,
                compact=compact, sort_dicts=sort_dicts)
    if stream is not None:
        stream.write(s + "\n")
    else:
        print(s)


def pp(obj, *args, **kwargs):
    """3.8+ alias for pprint with sort_dicts=False default."""
    kwargs.setdefault("sort_dicts", False)
    pprint(obj, *args, **kwargs)


def isreadable(obj):
    return True


def isrecursive(obj):
    return False


class PrettyPrinter:
    def __init__(self, indent=1, width=80, depth=None, stream=None,
                 *, compact=False, sort_dicts=True):
        self._indent = indent
        self._width = width
        self._depth = depth
        self._stream = stream
        self._compact = compact
        self._sort_dicts = sort_dicts
    def pprint(self, obj):
        pprint(obj, stream=self._stream, indent=self._indent,
               width=self._width, depth=self._depth,
               compact=self._compact, sort_dicts=self._sort_dicts)
    def pformat(self, obj):
        return pformat(obj, indent=self._indent, width=self._width,
                       depth=self._depth, compact=self._compact,
                       sort_dicts=self._sort_dicts)


__all__ = ["pprint", "pformat", "pp", "PrettyPrinter",
           "isreadable", "isrecursive"]
