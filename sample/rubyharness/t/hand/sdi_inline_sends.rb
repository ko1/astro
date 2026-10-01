# Frequent builtin sends that an AOT build runs in place (korb_sdi_tab): every receiver kind
# (nil / true / Integer / Float / Symbol / main / singleton / subclass), later redefinition,
# out-of-range and frozen cases.  The interpreter and an AOT build must agree with CRuby.
def run(xs, &b) = xs.map { |x| begin; b.call(x); rescue => e; e.class; end }
objs = [nil, true, false, 1, -5, 2.5, :sym, "str", "", [1, 2], [], { a: 1 }, {}, Object.new, self]
p run(objs) { _1.nil? }
p run(objs) { _1.class }
p run(objs) { _1.equal?(_1) }, run(objs) { _1.equal?(1) }
p run(objs) { _1.instance_of?(Integer) }, run(objs) { _1.instance_of?(Comparable) }
p run(objs) { _1.is_a?(Comparable) }, run(objs) { _1.kind_of?(Enumerable) }
p run(objs) { _1.frozen? }, run(objs) { _1.hash.class }, run(objs) { _1.to_s.sub(/0x\h+/, "0x") }
p run(objs) { _1.respond_to?(:each) }
p run(["", "ab", [], [3], {}, { k: 1 }]) { [_1.empty?, _1.size, _1.length] }
p run(["abc", "", "é"]) { [_1.bytesize, _1.start_with?("a"), _1.end_with?("c")] }
p run([[], [1, 2, 3]]) { [_1.first, _1.last, _1.include?(2)] }
p run([{ a: 1 }, {}]) { [_1.key?(:a), _1.include?(:a)] }
p run([0, 7, -7, -(2**62), 2**70]) { [_1.abs, _1.even?, _1.odd?, _1.zero?] }
s = +"abc"
p s.getbyte(0), s.getbyte(-1), s.getbyte(9), s.setbyte(0, 65), s, s.setbyte(-1, 300), s
begin; "x".freeze.setbyte(0, 1); rescue => e; p e.class; end
begin; "x".start_with?(1); rescue => e; p e.class; end
begin; 1.instance_of?(1); rescue => e; p e.class; end
o = Object.new
def o.nil? = :singleton_nil
def o.class = :singleton_class
p o.nil?, o.class
class Sub < Array
  def size = :sub_size
  def empty? = :sub_empty
end
p Sub.new.size, Sub.new.empty?, Sub.new.first
class BO < BasicObject; end
begin; BO.new.nil?; rescue NoMethodError => e; p e.class; end
def loop_site(xs) = xs.map { [_1.nil?, _1.size, _1.empty?, _1.abs] rescue $!.class }
p loop_site([[1], [2]]), loop_site([1, -2])
class Array
  def size = :redef_size
  def first = :redef_first
end
class Integer
  def abs = :redef_abs
end
class Object
  def nil? = :redef_nil
end
p loop_site([[1], [2]]), [1].first, -3.abs, Object.new.nil?, nil.nil?
# the same names on a class that defines them in Ruby (the site's cache then holds a Ruby
# method, which must not be taken for one of the builtins)
class Own
  def to_s = "own"
  def hash = 42
  def frozen? = :own
  def is_a?(k) = :own_is_a
  def respond_to?(m, priv = false) = :own_rt
  def key?(k) = :own_key
  def include?(x) = :own_inc
  def size = :own_size
  def empty? = :own_empty
  def first = :own_first
end
2.times { |i| p [Own.new.to_s, Own.new.hash, Own.new.frozen?, Own.new.is_a?(Own), Own.new.respond_to?(:x),
                 Own.new.key?(1), Own.new.include?(1), Own.new.size, Own.new.empty?, Own.new.first] }
# second batch: called directly (match? / upcase / downcase / dup / force_encoding / freeze /
# to_i / push / pop / join / <=> / to_sym), plus a Ruby redefinition at the end
p run(["Ab", :Sy, /a/]) { [_1.match?("A"), (_1.upcase rescue $!.class), (_1.downcase rescue $!.class)] }
p run([1, 2.5, "s", [1], Object.new]) { d = _1.dup; [d.class, d.equal?(_1) || d == _1] }
p run([+"ab"]) { [_1.force_encoding("BINARY").encoding, _1.freeze.frozen?] }
p run([7, "12x", nil, 3.9, "z"]) { _1.to_i }
p run([[1, 2]]) { |a| [a.push(3), a.pop, a.join, a.join("-")] }
p run([1, "b", :c, [1, 2], Object.new]) { _1 <=> _1 }, (1 <=> "x"), ("a" <=> "b"), ([1] <=> [2])
p run(["s", :t]) { _1.to_sym }
class String
  def upcase = :redef_up
end
p "x".upcase, :y.upcase
