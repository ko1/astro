"""pystro stub for `pyexpat` (XML parsing C extension)."""

EXPAT_VERSION = "expat_2.5.0"
version_info = (2, 5, 0)


class ExpatError(Exception):
    pass


error = ExpatError


def ParserCreate(encoding=None, namespace_separator=None, intern=None):
    raise ExpatError("pyexpat not supported in pystro")


class XMLParserType:
    pass


# Error codes (subset).
errors = type("ExpatErrors", (), {
    "XML_ERROR_NONE": 0,
    "XML_ERROR_NO_MEMORY": 1,
    "XML_ERROR_SYNTAX": 2,
    "XML_ERROR_NO_ELEMENTS": 3,
    "XML_ERROR_INVALID_TOKEN": 4,
    "messages": {},
})()


# pyexpat.model — content-model API constants (used by xml.parsers.expat.py
# which does `from pyexpat import *` then sys.modules[...] = model).
model = type("model", (), {
    "XML_CTYPE_EMPTY": 1, "XML_CTYPE_ANY": 2, "XML_CTYPE_MIXED": 3,
    "XML_CTYPE_NAME": 4, "XML_CTYPE_CHOICE": 5, "XML_CTYPE_SEQ": 6,
    "XML_CQUANT_NONE": 0, "XML_CQUANT_OPT": 1, "XML_CQUANT_REP": 2,
    "XML_CQUANT_PLUS": 3,
})()


__all__ = ["EXPAT_VERSION", "ExpatError", "error", "ParserCreate",
           "XMLParserType", "errors", "model"]
