# The implicit-conversion probes (#to_str / #to_ary / #to_hash) ask an object's own #respond_to? only when it
# is overridden: a plain object (and an Encoding) answers through the built-in one without a dispatch, while
# overrides of #respond_to? or #respond_to_missing? — public, private, in a module, added later — are still asked.
class Plain; end
class RtYes
  def respond_to?(m, priv = false) = (m == :to_str || m == :to_ary) || super
  def method_missing(m, *a) = m == :to_str ? "plain" : m == :to_ary ? [1, 2] : super
end
class RtmYes
  def respond_to_missing?(m, priv = false) = m == :to_str || m == :to_ary || super
  def method_missing(m, *a) = m == :to_str ? "plain" : m == :to_ary ? [3, 4] : super
end
module Asks; def respond_to?(m, priv = false) = ($asked << m; super); end
class Logged; include Asks; end
$asked = []
e = Encoding::UTF_8
3.times do |i|
  GC.start if i == 1
  p "plain" == Plain.new, "plain" == RtYes.new, "plain" == RtmYes.new, "UTF-8" == e, "x" == :x
  p [Plain.new].flatten.size, [RtYes.new].flatten, [RtmYes.new].flatten, [e].flatten
  p Array(e).size, Array(Plain.new).size
  p "x".encode("UTF-8", e).encoding, e.respond_to?(:to_str), Plain.new.respond_to?(:to_str)
end
[Logged.new].flatten; "a" == Logged.new
p $asked.uniq.sort
class Plain; def respond_to?(m, priv = false) = m == :to_str || super; def to_str = "plain"; end
p "plain" == Plain.new
class Plain; remove_method :respond_to?, :to_str; end
p "plain" == Plain.new
o = Object.new
def o.respond_to_missing?(m, priv) = m == :to_ary
def o.method_missing(m, *a) = m == :to_ary ? [:sing] : super
p [o].flatten
