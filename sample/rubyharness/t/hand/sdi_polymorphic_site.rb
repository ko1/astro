# one AOT inline-send site reached by many receiver classes: each class's own resolution of the
# name decides (builtin → inline answer; an override, private, refinement → the real send)
class Drop; def respond_to?(n, all = false) = n == :magic; end
class Plain; def m = 1; end
class Lying; def is_a?(k) = :lie; def instance_of?(k) = :lie; def nil? = :lie; def length = :lie; def size = :lie; end
class PrivLen < Array; private def length = 0; end
class MyStr < String; end
class MyHash < Hash; def size = :myhash; end
def probe(o)
  [o.respond_to?(:magic), o.respond_to?(:m), o.is_a?(Comparable), o.is_a?(Hash), o.instance_of?(String),
   o.nil?, o.class, (o.length rescue :nolen), (o.size rescue :nosize), (o.start_with?("a") rescue :nosw), (o.include?(1) rescue :noinc)]
end
objs = [{ a: 1 }, "abc", Drop.new, Plain.new, Lying.new, [1, 2], MyStr.new("ab"), MyHash.new, nil, 42, :sym, 1.5, PrivLen.new([1])]
3.times { objs.each { |o| p probe(o) } }
# a redefinition while the site is warm, then a removal
class String; alias __orig_len length; def length = :redef_len; end
p probe("x")
class String; alias length __orig_len; end
p probe("xy")
class Hash; def is_a?(k) = :hash_isa; end
p probe({})
class Hash; remove_method :is_a?; end
p probe({ b: 2 })
# refinements: a refined site sees the refinement, an unrefined one the builtin
module R; refine(Array) { def length = :refined }; end
def plain_len(o) = o.length
using R
def refined_len(o) = o.length
p [plain_len([1]), refined_len([1]), plain_len("ab"), refined_len("ab")]
