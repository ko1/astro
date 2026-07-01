# Array#slice! raises FrozenError upfront on a frozen array. vs ruby.
a = [1, 2, 3, 4, 5]
p a.slice!(1, 2)
p a
[1, 2, 3].freeze.tap { |b| begin; b.slice!(0); rescue => e; p e.class; end }
[1, 2, 3, 4, 5].freeze.tap { |c| begin; c.slice!(1..2); rescue => e; p e.class; end }
d = [1, 2, 3]
p d.slice!(0..1)
p d
p [1, 2, 3, 4].slice!(1, 2)
