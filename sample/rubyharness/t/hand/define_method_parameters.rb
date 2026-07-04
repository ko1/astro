# A define_method'd method reports its block's parameters (strict :req). vs ruby.
class C
  define_method(:one) { |x| }
  define_method(:opt) { |x = 1| }
  define_method(:rest) { |*x| }
  define_method(:mix) { |a, b = 1, *c, d, k:, m: 2, **n, &bl| }
end
p C.instance_method(:one).parameters
p C.instance_method(:opt).parameters
p C.instance_method(:rest).parameters
p C.instance_method(:mix).parameters
p C.new.method(:one).parameters
