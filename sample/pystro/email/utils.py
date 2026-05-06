"""pystro stub for `email.utils`."""


def quote(s):
    return '"' + str(s).replace('\\', '\\\\').replace('"', '\\"') + '"'


def unquote(s):
    s = str(s)
    if len(s) >= 2 and s[0] == '"' and s[-1] == '"':
        return s[1:-1].replace('\\"', '"').replace('\\\\', '\\')
    return s


def parseaddr(addr):
    if "@" in addr:
        local, _, domain = addr.partition("@")
        return ("", local + "@" + domain)
    return ("", addr)


def formataddr(pair, charset="utf-8"):
    name, addr = pair
    if name: return f"{name} <{addr}>"
    return addr


def getaddresses(fieldvalues):
    return [parseaddr(v) for v in fieldvalues]


def parsedate(data):
    return None


def parsedate_tz(data):
    return None


def parsedate_to_datetime(data):
    raise ValueError


def mktime_tz(data):
    return 0


def formatdate(timeval=None, localtime=False, usegmt=False):
    return ""


def format_datetime(dt, usegmt=False):
    return ""


def make_msgid(idstring=None, domain=None):
    return "<id@example>"


def collapse_rfc2231_value(value, errors="replace", fallback_charset="us-ascii"):
    return str(value)


def decode_rfc2231(s):
    return (None, None, s)


def encode_rfc2231(s, charset=None, language=None):
    return s


def decode_params(params):
    return params


__all__ = ["quote", "unquote", "parseaddr", "formataddr", "getaddresses",
           "parsedate", "parsedate_tz", "parsedate_to_datetime",
           "mktime_tz", "formatdate", "format_datetime", "make_msgid",
           "collapse_rfc2231_value", "decode_rfc2231", "encode_rfc2231",
           "decode_params"]
