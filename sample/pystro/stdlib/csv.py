# pystro stdlib `csv` (minimal).
#
# Supports basic reader/writer with comma delimiter and double-quote
# quoting.

QUOTE_MINIMAL = 0
QUOTE_ALL = 1
QUOTE_NONNUMERIC = 2
QUOTE_NONE = 3


class Dialect:
    delimiter = ","
    quotechar = '"'
    quoting = QUOTE_MINIMAL
    lineterminator = "\r\n"
    doublequote = True
    escapechar = None
    skipinitialspace = False
    def __init__(self, **kw):
        for k, v in kw.items():
            setattr(self, k, v)


excel = Dialect


def _parse_row(line, delim, quote):
    out = []
    i = 0
    n = len(line)
    cur = []
    in_quote = False
    while i < n:
        ch = line[i]
        if in_quote:
            if ch == quote:
                if i + 1 < n and line[i + 1] == quote:
                    cur.append(quote); i += 2; continue
                in_quote = False; i += 1; continue
            cur.append(ch); i += 1
        else:
            if ch == delim:
                out.append("".join(cur)); cur = []
                i += 1
            elif ch == quote:
                in_quote = True; i += 1
            else:
                cur.append(ch); i += 1
    out.append("".join(cur))
    return out


class reader:
    def __init__(self, source, dialect=None, **kwargs):
        self._source = source
        self.delim = kwargs.get("delimiter", ",")
        self.quote = kwargs.get("quotechar", '"')
        self._iter = iter(source)
    def __iter__(self):
        return self
    def __next__(self):
        line = next(self._iter)
        if line and line.endswith("\n"):
            line = line[:-1]
        if line and line.endswith("\r"):
            line = line[:-1]
        return _parse_row(line, self.delim, self.quote)


class writer:
    def __init__(self, fp, dialect=None, **kwargs):
        self.fp = fp
        self.delim = kwargs.get("delimiter", ",")
        self.quote = kwargs.get("quotechar", '"')
        self.term  = kwargs.get("lineterminator", "\r\n")
    def writerow(self, row):
        out = []
        for v in row:
            s = str(v)
            need_quote = self.delim in s or self.quote in s or "\n" in s or "\r" in s
            if need_quote:
                s = s.replace(self.quote, self.quote + self.quote)
                s = self.quote + s + self.quote
            out.append(s)
        self.fp.write(self.delim.join(out) + self.term)
    def writerows(self, rows):
        for r in rows:
            self.writerow(r)


class DictReader:
    def __init__(self, source, fieldnames=None, **kwargs):
        self._reader = reader(source, **kwargs)
        if fieldnames is None:
            try:
                fieldnames = next(self._reader)
            except StopIteration:
                fieldnames = []
        self.fieldnames = fieldnames
    def __iter__(self):
        return self
    def __next__(self):
        row = next(self._reader)
        d = {}
        for i, k in enumerate(self.fieldnames):
            d[k] = row[i] if i < len(row) else None
        return d


class DictWriter:
    def __init__(self, fp, fieldnames, **kwargs):
        self._writer = writer(fp, **kwargs)
        self.fieldnames = fieldnames
    def writeheader(self):
        self._writer.writerow(self.fieldnames)
    def writerow(self, d):
        self._writer.writerow([d.get(k, "") for k in self.fieldnames])
    def writerows(self, rows):
        for r in rows: self.writerow(r)


class Error(Exception):
    pass


def field_size_limit(*args):
    return 131072
