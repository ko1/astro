# puts on a self-referential array prints "[...]" at the recursion point
# instead of overflowing the stack. vs ruby.
x = []
x << 2 << x
puts x

y = [1, [2, 3]]
puts y

z = [1]
z << [z, 4]
puts z
