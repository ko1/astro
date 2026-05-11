# pystro stdlib `unittest.mock` — minimal stub.
#
# CPython's unittest.mock uses descriptor / __new__ / __class__ tricks
# pystro can't replicate.  This stub mirrors the API surface used by
# CPython's own test suite (Mock / MagicMock / patch / sentinel / ANY /
# call / mock_open / create_autospec) with no-op semantics.


class _MockSentinel:
    def __init__(self, name): self.name = name
    def __repr__(self): return "sentinel." + self.name


class _MockSentinelFactory:
    def __getattr__(self, name): return _MockSentinel(name)


class Mock:
    def __init__(self, *args, **kwargs):
        self._spec = kwargs.pop("spec", None)
        self._return_value = kwargs.pop("return_value", None)
        self._side_effect = kwargs.pop("side_effect", None)
        self._wraps = kwargs.pop("wraps", None)
        self._name = kwargs.pop("name", None)
        self.call_args = None
        self.call_args_list = []
        self.call_count = 0
        self.mock_calls = []
        self.method_calls = []
        self._children = {}
        for k, v in kwargs.items():
            try:
                setattr(self, k, v)
            except Exception:
                pass

    def __call__(self, *args, **kwargs):
        self.call_args = (args, kwargs)
        self.call_args_list.append(self.call_args)
        self.call_count += 1
        self.mock_calls.append((args, kwargs))
        if self._side_effect is not None:
            se = self._side_effect
            if isinstance(se, Exception) or (isinstance(se, type) and issubclass(se, BaseException)):
                raise se
            if callable(se):
                return se(*args, **kwargs)
            try:
                return next(se)
            except TypeError:
                return se
        if self._wraps is not None:
            return self._wraps(*args, **kwargs)
        return self._return_value

    def __getattr__(self, name):
        if name.startswith("_") or name in ("call_args", "call_args_list",
                                            "call_count", "mock_calls",
                                            "method_calls"):
            raise AttributeError(name)
        if name not in self._children:
            child = Mock()
            self._children[name] = child
        return self._children[name]

    def __iter__(self):
        return iter([])

    @property
    def return_value(self): return self._return_value
    @return_value.setter
    def return_value(self, v): self._return_value = v

    @property
    def side_effect(self): return self._side_effect
    @side_effect.setter
    def side_effect(self, v): self._side_effect = v

    def reset_mock(self, *a, **kw):
        self.call_args = None
        self.call_args_list = []
        self.call_count = 0
        self.mock_calls = []

    def configure_mock(self, **kwargs):
        for k, v in kwargs.items():
            setattr(self, k, v)

    def attach_mock(self, child, name):
        self._children[name] = child

    def assert_called(self):
        if self.call_count == 0:
            raise AssertionError("Expected to be called")
    def assert_called_once(self):
        if self.call_count != 1:
            raise AssertionError("Expected 1 call, got " + str(self.call_count))
    def assert_not_called(self):
        if self.call_count != 0:
            raise AssertionError("Expected no calls, got " + str(self.call_count))
    def assert_called_with(self, *args, **kwargs):
        if self.call_args != (args, kwargs):
            raise AssertionError("Expected " + repr((args, kwargs)) + ", got " + repr(self.call_args))
    def assert_called_once_with(self, *args, **kwargs):
        self.assert_called_once()
        self.assert_called_with(*args, **kwargs)
    def assert_any_call(self, *args, **kwargs):
        if (args, kwargs) not in self.call_args_list:
            raise AssertionError("call not found: " + repr((args, kwargs)))


# MagicMock / NonCallableMock / PropertyMock / AsyncMock are all aliases
# of the same minimal Mock above for pystro's purposes.
MagicMock = Mock
NonCallableMock = Mock
NonCallableMagicMock = Mock
PropertyMock = Mock
AsyncMock = Mock


class _PatchCM:
    def __init__(self, target, new=None, **kwargs):
        self.target = target
        self.new = new if new is not None else Mock()
        self._kwargs = kwargs
    def __enter__(self): return self.new
    def __exit__(self, *exc): return False
    def start(self): return self.new
    def stop(self): pass
    def __call__(self, fn):
        # decorator form: wrap fn so it receives the mock as the
        # first positional argument after self (when used on methods).
        def wrapper(*args, **kwargs):
            return fn(*args, self.new, **kwargs)
        return wrapper


def patch(target, *args, **kwargs):
    return _PatchCM(target, *args, **kwargs)


def _patch_object(target, attribute, *args, **kwargs):
    return _PatchCM(str(target) + "." + str(attribute), *args, **kwargs)


def _patch_dict(in_dict, values=(), clear=False):
    return _PatchCM("dict")


def _patch_multiple(target, **kwargs):
    return _PatchCM(target)


patch.object = _patch_object
patch.dict = _patch_dict
patch.multiple = _patch_multiple
patch.TEST_PREFIX = "test"
patch.stopall = lambda: None


class _Call(tuple):
    """Mimics unittest.mock.call — used as `call(a, b)` and `call.method(a)`."""
    def __new__(cls, args=(), kwargs=None, name=""):
        return tuple.__new__(cls, (args, kwargs or {}))
    def __init__(self, args=(), kwargs=None, name=""):
        self.name = name
    def __call__(self, *args, **kwargs):
        return _Call(args, kwargs)
    def __getattr__(self, name):
        if name.startswith("_"):
            raise AttributeError(name)
        return _Call(name=name)


call = _Call()
sentinel = _MockSentinelFactory()
DEFAULT = object()
ANY = object()


def create_autospec(spec, *args, **kwargs):
    return Mock()


def mock_open(read_data=""):
    m = Mock()
    m.read = Mock(return_value=read_data)
    m.readline = Mock(return_value=read_data)
    m.readlines = Mock(return_value=[read_data] if read_data else [])
    m.__enter__ = Mock(return_value=m)
    m.__exit__ = Mock(return_value=False)
    return m


def seal(mock):
    return mock


__all__ = ["Mock", "MagicMock", "NonCallableMock", "NonCallableMagicMock",
           "PropertyMock", "AsyncMock", "patch", "sentinel", "ANY",
           "DEFAULT", "call", "create_autospec", "mock_open", "seal"]
