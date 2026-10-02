# Klass.new through the small send entry, an optional-args method given every argument,
# and String#length / #size through the AOT inline send
class N
  def initialize(a, b = a + 1, c = :dflt) = (@v = [a, b, c])
  attr_reader :v
end
class NB
  def initialize(a, &blk) = (@v = blk ? blk.call(a) : [:noblk, a])
  attr_reader :v
end
class Plain; end
def make(i) = [N.new(i).v, N.new(i, 0).v, N.new(i, 0, :given).v, NB.new(i).v, NB.new(i) { _1 * 10 }.v, Plain.new.class]
3.times { |i| p make(i) }
class O; def m(a, b = 2, c = 3) = [a, b, c]; end
o = O.new
3.times { |i| p o.m(i), o.m(i, 5), o.m(i, 5, 6) }
begin
  o.m(1, 2, 3, 4)
rescue ArgumentError => e
  p e.message
end
begin
  N.new
rescue ArgumentError => e
  p e.message
end
# a class-level `new` redefinition after the site cached Class#new
def make_n(i) = N.new(i)
3.times { p make_n(1).v }
class N; def self.new(*a) = :custom_new; end
p make_n(1)
# private initialize stays callable through new; new on a class without a user initialize
class P2; private def initialize(x) = (@x = x); attr_reader :x; end
p P2.new(5).x
# String#length / #size (character counts), subclass and a redefinition
strs = ["abc", "日本語", "", "aéb", "x" * 100]
3.times { p strs.map(&:length), strs.map(&:size), strs.map { _1.length }, strs.map { _1.size } }
class MyStr < String; end
p MyStr.new("héllo").length, MyStr.new("héllo").size
class MyStr; def length = :overridden; end
p MyStr.new("abc").length, "abc".length
p [1, 2, 3].length, { a: 1 }.size
