# pystro stdlib `abc` (minimal).
#
# Pystro doesn't enforce abstract methods at instantiation time;
# we just provide the surface API.

class ABCMeta(type):
    pass


class ABC:
    pass


def abstractmethod(fn):
    fn.__isabstractmethod__ = True
    return fn


def abstractclassmethod(fn):
    return classmethod(fn)


def abstractstaticmethod(fn):
    return staticmethod(fn)


def abstractproperty(fn):
    return property(fn)


def get_cache_token():
    return 0
