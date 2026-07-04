# Array#join follows the #to_str → #to_ary → #to_s conversion chain where a nil
# result falls through, and honors an object's own #respond_to?. vs ruby.
class Proxy
  def initialize(m, v); @m = m; @v = v; end
  def respond_to?(n, p = false); n == @m; end
  def method_missing(n, *a); n == @m ? @v : super; end
end
class NilStr; def to_str; nil; end; def to_ary; ["A", "B"]; end; end
p ["x", Proxy.new(:to_str, "STR"), "y"].join("-")
p ["x", Proxy.new(:to_ary, ["p", "q"]), "y"].join("-")
p ["x", NilStr.new, "y"].join("-")
p [1, [2, [3, 4]], 5].join("-")
