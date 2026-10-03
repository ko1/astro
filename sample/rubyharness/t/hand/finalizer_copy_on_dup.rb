# #dup / #clone carry an object's finalizers to the copy; once any finalizer exists, a copy of an
# object without one must still have none (the copy path skips objects that never got an id)
class Res; attr_accessor :name; def initialize(n) = @name = n; end
$log = []
def fin(tag) = proc { |_id| $log << tag }
plain = Res.new(:plain)
s = +"str"
a = [1, 2]
o = Res.new(:o)
ObjectSpace.define_finalizer(s, fin(:s))
ObjectSpace.define_finalizer(a, fin(:a))
ObjectSpace.define_finalizer(o, fin(:o))
copies = [s.dup, s.clone, a.dup, o.clone, o.dup, plain.dup, "x".dup, [3].dup]
p copies.map(&:class)
# a copy of a copy carries them on
cc = copies[0].dup
p cc
ObjectSpace.undefine_finalizer(o)
p o.dup.name
at_exit { p $log.sort.tally.sort }
