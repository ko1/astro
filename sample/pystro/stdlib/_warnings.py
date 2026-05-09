"""pystro stub for `_warnings` (CPython C accelerator for `warnings`)."""

import sys


_filters = []
_defaultaction = "default"
_onceregistry = {}

# CPython's `warnings.py` reads `_warnings.filters` and other public
# names; provide non-underscore aliases.
filters = _filters
defaultaction = _defaultaction
onceregistry = _onceregistry


def warn(message, category=None, stacklevel=1, source=None):
    if category is None:
        category = Warning
    sys.stderr.write(f"{category.__name__}: {message}\n")


def warn_explicit(message, category, filename, lineno,
                  module=None, registry=None, module_globals=None,
                  source=None):
    sys.stderr.write(f"{filename}:{lineno}: {category.__name__}: {message}\n")


def filters_mutated(): pass


def _filters_mutated(): pass


__all__ = ["warn", "warn_explicit", "filters_mutated", "_filters_mutated",
           "_filters", "_defaultaction", "_onceregistry"]
