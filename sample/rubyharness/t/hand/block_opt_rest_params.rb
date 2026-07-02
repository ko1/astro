# block/proc/lambda params combining optionals and a rest, with defaults applied
# to omitted optionals. vs ruby.
f = ->(a, b = 1, *r) { [a, b, r] }
p f.call(9)
p f.call(9, 8)
p f.call(9, 8, 7, 6)
g = proc { |a, b = 2, *r| [a, b, r] }
p g.call(1)
p g.call(1, 2, 3, 4)
[[1, 2, 3, 4]].each { |a, b = 9, *r| p [a, b, r] }
[[5]].each { |a, b = 9, *r| p [a, b, r] }
class C; define_method(:m) { |a, b = 1, *r| [a, b, r] }; end
p C.new.m(10)
p C.new.m(10, 20, 30)
