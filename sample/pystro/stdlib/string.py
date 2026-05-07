# pystro stdlib `string` constants.

ascii_lowercase = "abcdefghijklmnopqrstuvwxyz"
ascii_uppercase = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
ascii_letters   = ascii_lowercase + ascii_uppercase
digits          = "0123456789"
hexdigits       = digits + "abcdef" + "ABCDEF"
octdigits       = "01234567"
punctuation     = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
whitespace      = " \t\n\r\v\f"
printable       = digits + ascii_letters + punctuation + whitespace

class Template:
    def __init__(self, template):
        self.template = template
    def substitute(self, mapping=None, **kws):
        m = dict(mapping) if mapping else {}
        m.update(kws)
        return self._sub(m, strict=True)
    def safe_substitute(self, mapping=None, **kws):
        m = dict(mapping) if mapping else {}
        m.update(kws)
        return self._sub(m, strict=False)
    def _sub(self, m, strict):
        s = self.template
        out = []
        i = 0
        while i < len(s):
            if s[i] == "$":
                if i + 1 < len(s) and s[i + 1] == "$":
                    out.append("$"); i += 2; continue
                j = i + 1
                if j < len(s) and s[j] == "{":
                    j += 1
                    start = j
                    while j < len(s) and s[j] != "}": j += 1
                    name = s[start:j]
                    j += 1  # past }
                else:
                    start = j
                    while j < len(s) and (s[j].isalnum() or s[j] == "_"):
                        j += 1
                    name = s[start:j]
                if name in m:
                    out.append(str(m[name]))
                elif strict:
                    raise KeyError(name)
                else:
                    out.append(s[i:j])
                i = j
            else:
                out.append(s[i]); i += 1
        return "".join(out)


def capwords(s, sep=None):
    return (sep or " ").join(w.capitalize() for w in s.split(sep))


__all__ = ["ascii_lowercase", "ascii_uppercase", "ascii_letters",
           "digits", "hexdigits", "octdigits",
           "punctuation", "whitespace", "printable",
           "Template", "capwords"]
