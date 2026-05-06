# pystro stdlib `urllib.parse` (minimal): URL parsing/encoding helpers.

# RFC 3986 unreserved chars (don't need percent-encoding) plus '/'.
_UNRESERVED = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                  "abcdefghijklmnopqrstuvwxyz"
                  "0123456789-_.~")
_HEX = "0123456789ABCDEF"


def quote(s, safe="/", encoding=None, errors=None):
    """Percent-encode string for URL inclusion."""
    if isinstance(s, bytes):
        b = s
    else:
        b = s.encode("utf-8") if encoding is None else s.encode(encoding)
    out = []
    safe_set = _UNRESERVED | set(safe)
    for c in b:
        ch = chr(c)
        if ch in safe_set:
            out.append(ch)
        else:
            out.append("%" + _HEX[c >> 4] + _HEX[c & 0xF])
    return "".join(out)


def quote_plus(s, safe="", encoding=None, errors=None):
    return quote(s, safe + " ", encoding, errors).replace(" ", "+")


def unquote(s, encoding="utf-8", errors="replace"):
    """Decode percent-encoded string."""
    if isinstance(s, bytes):
        s = s.decode("ascii")
    out = []
    i = 0
    n = len(s)
    while i < n:
        ch = s[i]
        if ch == "%" and i + 2 < n:
            try:
                out.append(chr(int(s[i+1:i+3], 16)))
                i += 3
                continue
            except ValueError:
                pass
        out.append(ch)
        i += 1
    return "".join(out)


def unquote_plus(s, encoding="utf-8", errors="replace"):
    return unquote(s.replace("+", " "), encoding, errors)


def urlencode(query, doseq=False, safe="", encoding=None, errors=None,
              quote_via=quote_plus):
    """Convert {k: v} or [(k, v), ...] to URL-encoded query string."""
    if hasattr(query, "items"):
        items = list(query.items())
    else:
        items = list(query)
    pairs = []
    for k, v in items:
        ks = quote_via(str(k), safe, encoding, errors)
        if doseq and isinstance(v, (list, tuple)):
            for item in v:
                pairs.append(ks + "=" + quote_via(str(item), safe, encoding, errors))
        else:
            pairs.append(ks + "=" + quote_via(str(v), safe, encoding, errors))
    return "&".join(pairs)


def parse_qs(qs, keep_blank_values=False, strict_parsing=False,
             encoding="utf-8", errors="replace", max_num_fields=None,
             separator="&"):
    """Parse query string into {key: [val, ...]}."""
    out = {}
    if not qs: return out
    for pair in qs.split(separator):
        if "=" in pair:
            k, _, v = pair.partition("=")
        else:
            k, v = pair, ""
        if not k and not keep_blank_values: continue
        if not v and not keep_blank_values: continue
        k = unquote_plus(k, encoding, errors)
        v = unquote_plus(v, encoding, errors)
        out.setdefault(k, []).append(v)
    return out


def parse_qsl(qs, keep_blank_values=False, strict_parsing=False,
              encoding="utf-8", errors="replace", max_num_fields=None,
              separator="&"):
    """Parse query string into [(key, val), ...]."""
    out = []
    if not qs: return out
    for pair in qs.split(separator):
        if "=" in pair:
            k, _, v = pair.partition("=")
        else:
            k, v = pair, ""
        if not k and not keep_blank_values: continue
        if not v and not keep_blank_values: continue
        out.append((unquote_plus(k, encoding, errors),
                    unquote_plus(v, encoding, errors)))
    return out


class _ParseResult:
    """Tuple-like (scheme, netloc, path, params, query, fragment)."""
    __slots__ = ("scheme", "netloc", "path", "params", "query", "fragment")

    def __init__(self, scheme, netloc, path, params, query, fragment):
        self.scheme = scheme
        self.netloc = netloc
        self.path = path
        self.params = params
        self.query = query
        self.fragment = fragment

    def __iter__(self):
        for f in self.__slots__: yield getattr(self, f)

    def __getitem__(self, i):
        if isinstance(i, str): return getattr(self, i)
        return list(self)[i]

    def __repr__(self):
        return "ParseResult(scheme={!r}, netloc={!r}, path={!r}, params={!r}, query={!r}, fragment={!r})".format(
            self.scheme, self.netloc, self.path, self.params, self.query, self.fragment)

    def __eq__(self, o):
        return tuple(self) == (tuple(o) if hasattr(o, "__iter__") else o)

    def __hash__(self):
        return hash(tuple(self))

    @property
    def hostname(self):
        n = self.netloc
        if "@" in n: n = n.split("@", 1)[1]
        if ":" in n: n = n.split(":", 1)[0]
        return n.lower() if n else None

    @property
    def port(self):
        n = self.netloc
        if "@" in n: n = n.split("@", 1)[1]
        if ":" in n:
            try: return int(n.rsplit(":", 1)[1])
            except ValueError: return None
        return None

    def geturl(self):
        return urlunparse(self)


ParseResult = _ParseResult


def urlparse(url, scheme="", allow_fragments=True):
    """Parse URL into 6 components."""
    s = url
    sch = scheme
    netloc = ""
    path = ""
    params = ""
    query = ""
    fragment = ""

    if "#" in s and allow_fragments:
        s, _, fragment = s.partition("#")

    if "?" in s:
        s, _, query = s.partition("?")

    # Scheme detection: scheme://...
    i = s.find(":")
    if i > 0 and all(c.isalnum() or c in "+-." for c in s[:i]):
        sch = s[:i]
        s = s[i+1:]

    if s.startswith("//"):
        s = s[2:]
        slash = s.find("/")
        if slash >= 0:
            netloc = s[:slash]
            s = s[slash:]
        else:
            netloc = s
            s = ""

    if ";" in s:
        s, _, params = s.partition(";")

    path = s
    return _ParseResult(sch, netloc, path, params, query, fragment)


def urlunparse(parts):
    if hasattr(parts, "scheme"):
        scheme, netloc, path, params, query, fragment = (
            parts.scheme, parts.netloc, parts.path, parts.params, parts.query, parts.fragment)
    else:
        scheme, netloc, path, params, query, fragment = parts
    out = []
    if scheme: out.append(scheme + ":")
    if netloc or scheme in ("http", "https", "ftp", "file"):
        out.append("//" + netloc)
    out.append(path)
    if params: out.append(";" + params)
    if query: out.append("?" + query)
    if fragment: out.append("#" + fragment)
    return "".join(out)


def urljoin(base, url, allow_fragments=True):
    """Join a base URL and a possibly-relative URL."""
    if not base: return url
    if not url: return base
    bp = urlparse(base)
    up = urlparse(url)
    # If url has scheme, return it as-is.
    if up.scheme and up.scheme != bp.scheme:
        return url
    # If url has netloc, use it directly.
    if up.netloc:
        return urlunparse(_ParseResult(bp.scheme, up.netloc, up.path, up.params,
                                       up.query, up.fragment))
    # Path resolution.
    if up.path.startswith("/"):
        path = up.path
    elif up.path:
        # Replace last segment of base path.
        base_path = bp.path
        slash = base_path.rfind("/")
        if slash >= 0:
            path = base_path[:slash + 1] + up.path
        else:
            path = up.path
    else:
        path = bp.path
    return urlunparse(_ParseResult(bp.scheme, bp.netloc, path,
                                   up.params or bp.params,
                                   up.query or bp.query,
                                   up.fragment))


__all__ = ["quote", "quote_plus", "unquote", "unquote_plus",
           "urlencode", "parse_qs", "parse_qsl",
           "urlparse", "urlunparse", "urljoin", "ParseResult"]
