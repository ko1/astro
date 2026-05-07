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


__all__ = ["EXPAT_VERSION", "ExpatError", "error", "ParserCreate",
           "XMLParserType", "errors"]
