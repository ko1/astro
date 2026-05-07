# pystro stdlib `argparse` (minimal).
#
# Supports:
#   parser = ArgumentParser(prog="...", description="...")
#   parser.add_argument("--flag", action="store_true")
#   parser.add_argument("--name", default="x", type=str)
#   parser.add_argument("-v", "--verbose", action="store_true")
#   parser.add_argument("path")            # positional
#   ns = parser.parse_args(["--flag", "--name=y", "input.txt"])
#   ns.flag, ns.name, ns.path

class _Namespace:
    def __init__(self):
        pass
    def __repr__(self):
        parts = []
        for k in dir(self):
            if k.startswith("_"):
                continue
            parts.append(k + "=" + repr(getattr(self, k)))
        return "Namespace(" + ", ".join(parts) + ")"


class _Argument:
    def __init__(self, names, action, default, type_, required, dest, help_, nargs, choices, const, metavar):
        self.names = names           # list of strings, e.g. ["-v", "--verbose"]
        self.action = action
        self.default = default
        self.type = type_
        self.required = required
        self.dest = dest
        self.help = help_
        self.nargs = nargs
        self.choices = choices
        self.const = const
        self.metavar = metavar

    def is_optional(self):
        return any(n.startswith("-") for n in self.names)

    def long_name(self):
        for n in self.names:
            if n.startswith("--"):
                return n[2:]
        for n in self.names:
            if n.startswith("-"):
                return n[1:]
        return self.names[0]


class ArgumentParser:
    def __init__(self, prog=None, usage=None, description=None,
                 epilog=None, parents=None, formatter_class=None,
                 prefix_chars="-", fromfile_prefix_chars=None,
                 argument_default=None, conflict_handler="error",
                 add_help=True, allow_abbrev=True, exit_on_error=True,
                 *, color=False, suggest_on_error=False):
        self.prog = prog or "prog"
        self.usage = usage
        self.description = description or ""
        self.epilog = epilog
        self.formatter_class = formatter_class or HelpFormatter
        self.prefix_chars = prefix_chars
        self.fromfile_prefix_chars = fromfile_prefix_chars
        self.argument_default = argument_default
        self.conflict_handler = conflict_handler
        self.add_help = add_help
        self.allow_abbrev = allow_abbrev
        self.exit_on_error = exit_on_error
        self._args = []          # list of _Argument

    def add_argument(self, *names, action=None, default=None, type=None,
                     required=False, dest=None, help=None, nargs=None,
                     choices=None, const=None, metavar=None):
        if not names:
            raise ValueError("add_argument: need a name")
        arg = _Argument(list(names), action, default, type, required, dest, help,
                        nargs, choices, const, metavar)
        if dest is None:
            n = arg.long_name()
            n = n.replace("-", "_")
            arg.dest = n
        self._args.append(arg)
        return arg

    def parse_args(self, argv):
        ns = _Namespace()
        # Initialize defaults.
        for a in self._args:
            if a.action == "store_true":
                setattr(ns, a.dest, False if a.default is None else a.default)
            elif a.action == "store_false":
                setattr(ns, a.dest, True if a.default is None else a.default)
            else:
                setattr(ns, a.dest, a.default)

        # Build option lookup.
        positionals = [a for a in self._args if not a.is_optional()]
        opt_by_flag = {}
        for a in self._args:
            if a.is_optional():
                for n in a.names:
                    opt_by_flag[n] = a

        seen = set()
        i = 0
        pos_idx = 0
        while i < len(argv):
            tok = argv[i]
            if tok.startswith("--"):
                eq = tok.find("=")
                if eq >= 0:
                    flag = tok[:eq]
                    val_str = tok[eq+1:]
                    a = opt_by_flag.get(flag)
                    if a is None:
                        raise ValueError("unknown option: " + flag)
                    seen.add(a.dest)
                    self._consume_inline(ns, a, val_str)
                    i += 1
                else:
                    a = opt_by_flag.get(tok)
                    if a is None:
                        raise ValueError("unknown option: " + tok)
                    seen.add(a.dest)
                    if a.action == "store_true":
                        setattr(ns, a.dest, True); i += 1
                    elif a.action == "store_false":
                        setattr(ns, a.dest, False); i += 1
                    else:
                        if i + 1 >= len(argv):
                            raise ValueError(tok + " requires a value")
                        self._consume_inline(ns, a, argv[i+1])
                        i += 2
            elif tok.startswith("-") and len(tok) > 1:
                a = opt_by_flag.get(tok)
                if a is None:
                    raise ValueError("unknown option: " + tok)
                seen.add(a.dest)
                if a.action == "store_true":
                    setattr(ns, a.dest, True); i += 1
                elif a.action == "store_false":
                    setattr(ns, a.dest, False); i += 1
                else:
                    if i + 1 >= len(argv):
                        raise ValueError(tok + " requires a value")
                    self._consume_inline(ns, a, argv[i+1])
                    i += 2
            else:
                # positional
                if pos_idx >= len(positionals):
                    raise ValueError("unexpected positional: " + tok)
                a = positionals[pos_idx]
                if a.nargs == "+" or a.nargs == "*":
                    # Greedy: consume all remaining non-option tokens (and
                    # any explicit numbers) until end or next option.
                    vals = [tok]
                    j = i + 1
                    while j < len(argv):
                        nx = argv[j]
                        if nx.startswith("-") and len(nx) > 1 and nx in opt_by_flag:
                            break
                        vals.append(nx)
                        j += 1
                    typed = [self._convert(a, v) for v in vals]
                    if a.nargs == "+" and len(typed) == 0:
                        raise ValueError("expected at least one " + a.names[0])
                    setattr(ns, a.dest, typed)
                    seen.add(a.dest)
                    pos_idx += 1
                    i = j
                    continue
                self._consume_inline(ns, a, tok)
                seen.add(a.dest)
                pos_idx += 1
                i += 1

        # Required check.
        for a in self._args:
            if a.required and a.dest not in seen:
                raise ValueError("missing required: " + (a.names[0]))
        # Positional must all be consumed unless they have defaults.
        if pos_idx < len(positionals):
            for a in positionals[pos_idx:]:
                if a.default is None:
                    raise ValueError("missing positional: " + a.names[0])

        return ns

    def _consume_inline(self, ns, a, val_str):
        v = self._convert(a, val_str)
        if a.choices and v not in a.choices:
            import sys
            sys.stderr.write("argparse: invalid choice " + repr(v) + "\n")
            raise SystemExit(2)
        setattr(ns, a.dest, v)

    def _convert(self, a, val_str):
        if a.type is None or a.type == str:
            return val_str
        if a.type == int:
            return int(val_str)
        if a.type == float:
            return float(val_str)
        return a.type(val_str)


class HelpFormatter:
    def __init__(self, prog, indent_increment=2, max_help_position=24, width=None):
        self.prog = prog
    def format_help(self): return ""
    def add_text(self, text): pass
    def add_usage(self, usage, actions, groups, prefix=None): pass
    def add_argument(self, action): pass
    def add_arguments(self, actions): pass
    def start_section(self, heading): pass
    def end_section(self): pass


class RawDescriptionHelpFormatter(HelpFormatter): pass
class RawTextHelpFormatter(HelpFormatter): pass
class ArgumentDefaultsHelpFormatter(HelpFormatter): pass
class MetavarTypeHelpFormatter(HelpFormatter): pass


class Action:
    def __init__(self, option_strings=None, dest=None, nargs=None, const=None,
                 default=None, type=None, choices=None, required=False,
                 help=None, metavar=None):
        self.option_strings = option_strings or []
        self.dest = dest
        self.nargs = nargs
        self.const = const
        self.default = default
        self.type = type
        self.choices = choices
        self.required = required
        self.help = help
        self.metavar = metavar


class _StoreAction(Action): pass
class _StoreConstAction(Action): pass
class _StoreTrueAction(Action): pass
class _StoreFalseAction(Action): pass
class _AppendAction(Action): pass
class _AppendConstAction(Action): pass
class _CountAction(Action): pass
class _HelpAction(Action): pass
class _VersionAction(Action): pass
class _SubParsersAction(Action): pass


class Namespace:
    def __init__(self, **kwargs):
        for k, v in kwargs.items():
            setattr(self, k, v)
    def __eq__(self, other):
        return self.__dict__ == getattr(other, "__dict__", None)


SUPPRESS = "==SUPPRESS=="
OPTIONAL = "?"
ZERO_OR_MORE = "*"
ONE_OR_MORE = "+"
PARSER = "A..."
REMAINDER = "..."


class ArgumentError(Exception):
    pass


class ArgumentTypeError(Exception):
    pass


class FileType:
    def __init__(self, mode="r", bufsize=-1, encoding=None, errors=None):
        self.mode = mode
    def __call__(self, string): return open(string, self.mode)


__all__ = ["ArgumentParser", "HelpFormatter", "RawDescriptionHelpFormatter",
           "RawTextHelpFormatter", "ArgumentDefaultsHelpFormatter",
           "MetavarTypeHelpFormatter", "Action", "Namespace",
           "ArgumentError", "ArgumentTypeError", "FileType",
           "SUPPRESS", "OPTIONAL", "ZERO_OR_MORE", "ONE_OR_MORE",
           "PARSER", "REMAINDER"]
