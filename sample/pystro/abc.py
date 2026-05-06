# pystro stdlib `abc` — abstract base classes.

class ABCMeta(type):
    @classmethod
    def register(cls, subclass):
        """Track a virtual subclass relationship.  pystro doesn't have
        a real ABC machinery, but we maintain a `_abc_registry` set on
        the class and have `bi_issubclass` consult it.  This makes
        `issubclass(C, ABCBase)` (and isinstance via type(v)) work for
        the registered virtual relationship.

        Pystro classes inherit dict entries via MRO, so naive
        `_abc_registry` would alias across the hierarchy.  Use a
        per-class name (`_abc_registry_<id>`) and the runtime knows to
        look for that exact attribute, not inherited ones."""
        # Per-class own-only attribute: store as `_abc_registry` but only
        # populate the set on `cls` itself; runtime walks consult this
        # attr by name on the specific class object.
        own = cls.__dict__.get("_abc_registry") if hasattr(cls.__dict__, "get") else None
        if own is None:
            own = set()
            cls._abc_registry = own
        own.add(subclass)
        return subclass

    @classmethod
    def __instancecheck__(cls, instance):
        # Real subclass first.
        try:
            if isinstance(instance, type):
                pass
            elif type.__instancecheck__(cls, instance):
                return True
        except Exception:
            pass
        # Walk MRO of (type(instance)) and check virtual registry.
        return cls.__subclasscheck__(type(instance))

    @classmethod
    def __subclasscheck__(cls, C):
        # Real subclass first.
        try:
            if type.__subclasscheck__(cls, C):
                return True
        except Exception:
            pass
        # Virtual registry.
        if hasattr(cls, "_abc_registry"):
            if C in cls._abc_registry:
                return True
            for reg in cls._abc_registry:
                try:
                    if isinstance(reg, type) and issubclass(C, reg):
                        return True
                except Exception:
                    pass
        # Walk bases recursively.
        try:
            bases = getattr(cls, "__bases__", ())
        except Exception:
            bases = ()
        for base in bases:
            if isinstance(base, ABCMeta):
                if base.__subclasscheck__(C):
                    return True
        return False

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
