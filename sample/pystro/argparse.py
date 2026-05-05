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
    def __init__(self, names, action, default, type_, required, dest, help_, nargs):
        self.names = names           # list of strings, e.g. ["-v", "--verbose"]
        self.action = action
        self.default = default
        self.type = type_
        self.required = required
        self.dest = dest
        self.help = help_
        self.nargs = nargs

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
    def __init__(self, prog=None, description=None):
        self.prog = prog or "prog"
        self.description = description or ""
        self._args = []          # list of _Argument

    def add_argument(self, *names, action=None, default=None, type=None,
                     required=False, dest=None, help=None, nargs=None):
        if not names:
            raise ValueError("add_argument: need a name")
        arg = _Argument(list(names), action, default, type, required, dest, help, nargs)
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
        if a.type is None or a.type == str:
            v = val_str
        elif a.type == int:
            v = int(val_str)
        elif a.type == float:
            v = float(val_str)
        else:
            v = a.type(val_str)
        setattr(ns, a.dest, v)


__all__ = ["ArgumentParser"]
