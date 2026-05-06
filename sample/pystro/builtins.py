"""pystro stub for `builtins` (CPython's namespace of everything in
default scope).  Code that does `builtins.print(...)` or `from builtins
import int, float, ...` should work.

We populate this module's globals from the current namespace at import
time using a runtime helper (since pystro's builtins ARE the global
namespace of the importing module too).
"""

# Numeric / sequence types — already in default scope.
int        = int
float      = float
complex    = complex
bool       = bool
str        = str
bytes      = bytes
bytearray  = bytearray
list       = list
tuple      = tuple
dict       = dict
set        = set
frozenset  = frozenset
range      = range
type       = type
object     = object
slice      = slice
memoryview = memoryview

# Singletons (CPython exposes True / False / None as attributes too,
# but in pystro they're keywords — assignment isn't allowed).

# Functions.
abs       = abs
all       = all
any       = any
ascii     = ascii
bin       = bin
breakpoint = breakpoint
callable  = callable
chr       = chr
classmethod = classmethod
compile   = compile
delattr   = delattr
dir       = dir
divmod    = divmod
enumerate = enumerate
eval      = eval
exec      = exec
filter    = filter
format    = format
getattr   = getattr
globals   = globals
hasattr   = hasattr
hash      = hash
hex       = hex
id        = id
input     = input
isinstance = isinstance
issubclass = issubclass
iter      = iter
len       = len
locals    = locals
map       = map
max       = max
min       = min
next      = next
oct       = oct
open      = open
ord       = ord
pow       = pow
print     = print
property  = property
repr      = repr
reversed  = reversed
round     = round
setattr   = setattr
sorted    = sorted
staticmethod = staticmethod
sum       = sum
vars      = vars
zip       = zip

# Exceptions.
BaseException = BaseException
Exception = Exception
ArithmeticError = ArithmeticError
AssertionError = AssertionError
AttributeError = AttributeError
BlockingIOError = BlockingIOError
BrokenPipeError = BrokenPipeError
BufferError = BufferError
BytesWarning = BytesWarning
ChildProcessError = ChildProcessError
ConnectionError = ConnectionError
DeprecationWarning = DeprecationWarning
EOFError = EOFError
EnvironmentError = EnvironmentError
ExceptionGroup = ExceptionGroup
BaseExceptionGroup = BaseExceptionGroup
FileNotFoundError = FileNotFoundError
FloatingPointError = FloatingPointError
FutureWarning = FutureWarning
GeneratorExit = GeneratorExit
IOError = IOError
ImportError = ImportError
ImportWarning = ImportWarning
IndentationError = IndentationError
IndexError = IndexError
InterruptedError = InterruptedError
IsADirectoryError = IsADirectoryError
KeyError = KeyError
KeyboardInterrupt = KeyboardInterrupt
LookupError = LookupError
MemoryError = MemoryError
ModuleNotFoundError = ModuleNotFoundError
NameError = NameError
NotADirectoryError = NotADirectoryError
NotImplementedError = NotImplementedError
OSError = OSError
OverflowError = OverflowError
PendingDeprecationWarning = PendingDeprecationWarning
PermissionError = PermissionError
RecursionError = RecursionError
ReferenceError = ReferenceError
ResourceWarning = ResourceWarning
RuntimeError = RuntimeError
RuntimeWarning = RuntimeWarning
StopAsyncIteration = StopAsyncIteration
StopIteration = StopIteration
SyntaxError = SyntaxError
SyntaxWarning = SyntaxWarning
SystemError = SystemError
SystemExit = SystemExit
TabError = TabError
TimeoutError = TimeoutError
TypeError = TypeError
UnboundLocalError = UnboundLocalError
UnicodeDecodeError = UnicodeDecodeError
UnicodeEncodeError = UnicodeEncodeError
UnicodeError = UnicodeError
UnicodeWarning = UnicodeWarning
UserWarning = UserWarning
ValueError = ValueError
Warning = Warning
ZeroDivisionError = ZeroDivisionError

__name__ = "builtins"

# Common typing-module names re-exposed at module-import-time so
# annotation expressions like `x: ClassVar[T]` don't fail when the
# user forgot `from typing import ClassVar`.  pystro doesn't enforce
# annotations at runtime, so these are just no-op subscript-passthrough.
class _AnnotationPassthrough:
    def __init__(self, name): self._name = name
    def __getitem__(self, params): return self
    def __repr__(self): return self._name
    def __call__(self, *a, **kw): return self if not a else a[0]
ClassVar = _AnnotationPassthrough("ClassVar")
Final = _AnnotationPassthrough("Final")
Literal = _AnnotationPassthrough("Literal")
Annotated = _AnnotationPassthrough("Annotated")
Self = _AnnotationPassthrough("Self")
TypeAlias = _AnnotationPassthrough("TypeAlias")
LiteralString = _AnnotationPassthrough("LiteralString")
NoReturn = _AnnotationPassthrough("NoReturn")
