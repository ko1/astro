# A class with many methods gets a hash index over its method table: lookups,
# remove_method, redefinition after removal, alias, undef, define_method and
# module lookups through it must answer as the plain scan did.
class Big
  60.times { |i| define_method(:"m#{i}") { i } }
end
b = Big.new
p b.m0, b.m59, b.respond_to?(:m30), b.respond_to?(:nope), Big.instance_methods(false).size
class Big
  remove_method :m10
  alias_method :m10b, :m11
  undef_method :m12
end
p b.respond_to?(:m10), b.m10b, (b.m12 rescue $!.class), Big.method_defined?(:m12)
class Big
  def m10 = :again
  define_method(:m60) { :sixty }
  61.upto(120) { |i| define_method(:"m#{i}") { -i } }
end
p b.m10, b.m60, b.m120, b.m61, Big.instance_methods(false).sort.first(3)
module Mix
  def m5 = :mix
  def only_mix = :only
end
class Big2 < Big
  include Mix
  def m7 = super * 100
end
b2 = Big2.new
p b2.m5, b2.m7, b2.only_mix, b2.m100, Big2.instance_method(:m5).owner
class Big
  remove_method :m7
end
p((b2.m7 rescue $!.class), b.respond_to?(:m7))
o = Object.new
def o.x1 = 1
40.times { |i| o.define_singleton_method(:"s#{i}") { i * 2 } }
p o.s39, o.singleton_methods.size, o.x1
class << o; remove_method :s3; end
p o.respond_to?(:s3), o.s4
