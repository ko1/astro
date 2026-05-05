# Dotted imports + `from m import *`.

# Dotted with `as` alias.
import pkg.sub.deep as d
print(d.hello())
print(d.PI_DEEP)

# `from a.b.c import name`.
from pkg.sub.deep import hello, PI_DEEP
print(hello())
print(PI_DEEP)

# `from a.b.c import name as alias`.
from pkg.sub.deep import hello as greet
print(greet())

# `from m import *` honours __all__.
from utilmod import *
print(PI)
print(square(5))
# `cube` is in utilmod but NOT in __all__ — should not be imported.
try:
    print(cube(3))
except NameError:
    print("cube not imported")

# Re-import the same module — should hit cache or at least work.
from utilmod import PI as P2
print(P2)
