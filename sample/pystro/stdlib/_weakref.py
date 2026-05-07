"""pystro stub for `_weakref` (the C accelerator for `weakref`)."""

from weakref import ref, ReferenceType, ProxyType, CallableProxyType, getweakrefcount, getweakrefs


def proxy(obj, callback=None):
    return obj


__all__ = ["ref", "ReferenceType", "ProxyType", "CallableProxyType",
           "getweakrefcount", "getweakrefs", "proxy"]
