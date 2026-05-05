import mathmod
print(mathmod.PI)
print(mathmod.square(7))

# `import as`
import mathmod as m
print(m.PI)
print(m.square(11))

# from import
from mathmod import square
print(square(3))

# from with alias
from mathmod import square as sq
print(sq(4))
