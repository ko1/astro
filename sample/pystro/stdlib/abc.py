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
        # Propagate registration to ABCMeta parents so issubclass(X,
        # Real) sees `Integral.register(X)`'s effect — pystro's
        # __subclasscheck__ walks bases (UP the chain) only.
        # Note: walk __bases__ recursively, not __mro__ (slicing the
        # synthetic __mro__ tuple-descriptor was a previous segv source).
        try:
            seen = set([id(cls)])
            todo = list(getattr(cls, "__bases__", ()) or ())
            while todo:
                b = todo.pop()
                if id(b) in seen:
                    continue
                seen.add(id(b))
                if not isinstance(b, ABCMeta):
                    continue
                bd = b.__dict__
                if not hasattr(bd, "get"):
                    continue
                bown = bd.get("_abc_registry")
                if bown is None:
                    bown = set()
                    b._abc_registry = bown
                bown.add(subclass)
                bb = getattr(b, "__bases__", None)
                if bb:
                    for x in bb: todo.append(x)
        except Exception:
            pass
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


# Note: a metaclass `__call__` would let ABCMeta intercept every
# instantiation, but pystro's `type.__call__` isn't usable from Python
# so we'd have to reimplement allocation + init by hand.  Doing so
# triggered regressions in unrelated stdlib tests (5 MIXED → TIMEOUT /
# CRASH), so the abstract-method check lives only on `ABC.__new__`.
# Tests that use `metaclass=ABCMeta` directly miss the runtime check
# but `_collect_abstracts` is still available for callers that want
# `__abstractmethods__` populated.


def _collect_abstracts(cls):
    """Walk the MRO and return the set of method names still abstract on
    `cls` (i.e. defined as abstract somewhere and not overridden by a
    concrete impl closer to `cls` in the MRO)."""
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
        items = bdict.items() if hasattr(bdict, "items") else []
        for name, val in items:
            if name in seen:
                continue
            # Pystro stashes per-decorator temporaries in the class body
            # under synthetic `__pystro_dec$N$<target>__` names so the
            # decorator can be applied AFTER the def runs.  These leak
            # into __dict__ but aren't user-visible methods, so skip.
            if name.startswith("__pystro_dec"):
                continue
            seen.add(name)
            if getattr(val, "__isabstractmethod__", False):
                abstracts.add(name)
    # Pystro stores the cached set on the class so consumers like
    # `inspect.isabstract` can pick it up.  Same name CPython uses.
    try:
        cls.__abstractmethods__ = frozenset(abstracts)
    except Exception:
        pass
    return abstracts


def _abc_caches_clear(cls): pass
def _abc_init(cls): pass
def _abc_register(cls, sub): return sub
def _abc_registry(cls): return frozenset()
def _abc_subclasshook(cls, sub): return NotImplemented
def update_abstractmethods(cls): return cls


class ABC(metaclass=ABCMeta):
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
                if name in seen or name.startswith("__pystro_dec"):
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


class abstractclassmethod(classmethod):
    """Deprecated form: use @classmethod + @abstractmethod instead.
    Pystro keeps the legacy class for parity with CPython tests."""
    __isabstractmethod__ = True
    def __init__(self, callable):
        callable.__isabstractmethod__ = True
        super().__init__(callable)


class abstractstaticmethod(staticmethod):
    __isabstractmethod__ = True
    def __init__(self, callable):
        callable.__isabstractmethod__ = True
        super().__init__(callable)


class abstractproperty(property):
    __isabstractmethod__ = True


def get_cache_token():
    return 0
