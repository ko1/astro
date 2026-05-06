# pystro stdlib `abc` — abstract base classes.

class ABCMeta(type):
    @classmethod
    def register(cls, subclass):
        """No-op subclass registration — pystro doesn't track virtual
        subclasses, so the registered class isn't recognised by
        isinstance unless it's already a real subclass."""
        return subclass

    @classmethod
    def __subclasshook__(cls, C):
        return NotImplemented


def _abc_caches_clear(cls): pass
def _abc_init(cls): pass
def _abc_register(cls, sub): return sub
def _abc_registry(cls): return frozenset()
def _abc_subclasshook(cls, sub): return NotImplemented
def update_abstractmethods(cls): return cls


class ABC:
    # Enforce abstract methods at instantiation: any method on the
    # class (or its bases) marked __isabstractmethod__ that isn't
    # overridden by the concrete subclass forbids instantiation.
    def __new__(cls, *args, **kwargs):
        # Walk the MRO collecting names where __isabstractmethod__ is True
        # and not overridden by a concrete impl earlier in the MRO.
        abstracts = set()
        seen = set()
        try:
            mro = cls.__mro__
        except AttributeError:
            mro = [cls]
        for base in mro:
            try:
                bdict = base.__dict__
            except Exception:
                continue
            for name, val in bdict.items() if hasattr(bdict, "items") else []:
                if name in seen:
                    continue
                seen.add(name)
                if getattr(val, "__isabstractmethod__", False):
                    abstracts.add(name)
        if abstracts:
            names = ", ".join(sorted(abstracts))
            plural = "s" if len(abstracts) > 1 else ""
            msg = "Can't instantiate abstract class " + cls.__name__
            msg += " with abstract method" + plural + " " + names
            raise TypeError(msg)
        return object.__new__(cls)


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
