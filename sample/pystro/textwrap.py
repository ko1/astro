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


def fill(text, width=70):
    return "\n".join(wrap(text, width))


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
