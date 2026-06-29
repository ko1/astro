a = [1, 2, 3, 4, 5]
srand(42)
r = a.shuffle!
p r.sort
p a.equal?(r)
p a.sort
def t; yield; rescue FrozenError; "FE"; end
p t { [1, 2].freeze.shuffle! }
srand(100)
x = [1, 2, 3, 4, 5].shuffle
srand(100)
y = [1, 2, 3, 4, 5].dup
y.shuffle!
p x == y
