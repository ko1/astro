# pystro stdlib `textwrap` (minimal).

def wrap(text, width=70):
    out = []
    for paragraph in text.split("\n"):
        line = ""
        for word in paragraph.split():
            if not line:
                line = word
            elif len(line) + 1 + len(word) <= width:
                line = line + " " + word
            else:
                out.append(line)
                line = word
        if line:
            out.append(line)
    return out


def fill(text, width=70, **kwargs):
    """CPython exposes initial_indent / subsequent_indent / break_long_words
    etc. via kwargs; pystro's stub honours initial_indent / subsequent_indent
    if given, ignores the rest."""
    init = kwargs.get("initial_indent", "")
    subs = kwargs.get("subsequent_indent", "")
    lines = wrap(text, width)
    if init or subs:
        if not lines:
            lines = [""]
        out = []
        for i, line in enumerate(lines):
            out.append((init if i == 0 else subs) + line)
        lines = out
    return "\n".join(lines)


def dedent(text):
    lines = text.split("\n")
    common = None
    for line in lines:
        stripped = line.lstrip()
        if not stripped:
            continue
        prefix = line[:len(line) - len(stripped)]
        if common is None:
            common = prefix
        else:
            new_common = ""
            for i in range(min(len(common), len(prefix))):
                if common[i] == prefix[i]:
                    new_common = new_common + common[i]
                else:
                    break
            common = new_common
    if not common:
        return text
    n = len(common)
    return "\n".join(line[n:] if line.startswith(common) else line for line in lines)


def indent(text, prefix, predicate=None):
    out = []
    for line in text.split("\n"):
        if predicate is None:
            if line:
                out.append(prefix + line)
            else:
                out.append(line)
        elif predicate(line):
            out.append(prefix + line)
        else:
            out.append(line)
    return "\n".join(out)


def shorten(text, width, placeholder=" [...]"):
    s = " ".join(text.split())
    if len(s) <= width:
        return s
    return s[:width - len(placeholder)].rstrip() + placeholder


class TextWrapper:
    """Minimal TextWrapper shim — exposes constructor kwargs and the
    fill/wrap methods over the module-level helpers."""
    def __init__(self, width=70, initial_indent="", subsequent_indent="",
                 expand_tabs=True, replace_whitespace=True,
                 fix_sentence_endings=False, break_long_words=True,
                 drop_whitespace=True, break_on_hyphens=True,
                 tabsize=8, max_lines=None, placeholder=" [...]", **kw):
        self.width = width
        self.initial_indent = initial_indent
        self.subsequent_indent = subsequent_indent
        self.expand_tabs = expand_tabs
        self.replace_whitespace = replace_whitespace
        self.fix_sentence_endings = fix_sentence_endings
        self.break_long_words = break_long_words
        self.drop_whitespace = drop_whitespace
        self.break_on_hyphens = break_on_hyphens
        self.tabsize = tabsize
        self.max_lines = max_lines
        self.placeholder = placeholder
    def wrap(self, text):
        return wrap(text, self.width)
    def fill(self, text):
        return fill(text, self.width)
