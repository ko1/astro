"""pystro stub for `_locale`."""

LC_ALL = 6
LC_COLLATE = 3
LC_CTYPE = 0
LC_MESSAGES = 5
LC_MONETARY = 4
LC_NUMERIC = 1
LC_TIME = 2

CHAR_MAX = 127


class Error(Exception):
    pass


_loc = "C"


def setlocale(category, locale=None):
    global _loc
    if locale is not None and locale != "":
        _loc = locale
    return _loc


def getlocale(category=LC_ALL):
    return (None, None)


def localeconv():
    return {
        "decimal_point": ".",
        "thousands_sep": "",
        "grouping": [],
        "int_curr_symbol": "",
        "currency_symbol": "",
        "mon_decimal_point": "",
        "mon_thousands_sep": "",
        "mon_grouping": [],
        "positive_sign": "",
        "negative_sign": "-",
        "int_frac_digits": 127,
        "frac_digits": 127,
        "p_cs_precedes": 127,
        "p_sep_by_space": 127,
        "n_cs_precedes": 127,
        "n_sep_by_space": 127,
        "p_sign_posn": 127,
        "n_sign_posn": 127,
    }


def nl_langinfo(item):
    return ""


def strcoll(a, b):
    if a < b: return -1
    if a > b: return 1
    return 0


def strxfrm(s): return s


def getencoding():
    return "utf-8"


def _getdefaultlocale():
    return (None, None)


__all__ = ["Error", "setlocale", "getlocale", "localeconv", "nl_langinfo",
           "strcoll", "strxfrm", "LC_ALL", "LC_COLLATE", "LC_CTYPE",
           "LC_MESSAGES", "LC_MONETARY", "LC_NUMERIC", "LC_TIME",
           "CHAR_MAX", "getencoding"]
