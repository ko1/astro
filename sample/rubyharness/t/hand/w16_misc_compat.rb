# Shapes the w16 gem corpus hit (bindata, gammo, CFPropertyList).

# `class ::X` inside instance_eval(String) on a singleton scope is top level
# (bindata's big_and_little endian makes ::FooBe / ::FooLe this way)
module Mk
  class << self
    def mk(n) = instance_eval "class ::#{n}Be; def self.tag = :be; end"
    def mkm(n) = instance_eval "module ::#{n}Mod; end"
  end
end
Mk.mk("W16x")
Mk.mkm("W16x")
p [W16xBe.name, W16xBe.tag, W16xMod.name, Object.const_defined?(:W16xBe)]

# each_slice / each_cons / each_char enumerators: `.each {}` / `.with_index {}`
# with a block give the receiver back, as the direct call does (gammo)
a = [1, 2, 3, 4, 5]
p [a.each_slice(2).each { }.equal?(a), a.each_cons(2).each { }.equal?(a),
   a.each_slice(2).with_index { }.equal?(a), a.each_slice(2).size]
s = +"abc"
p [s.each_char.each { }.equal?(s), s.each_char.with_index { }.equal?(s), s.each_char.with_index.to_a]

# String#== with an object whose #to_str comes from respond_to_missing? asks it back
class StrLike
  def respond_to_missing?(n, priv = false) = n == :to_str || super
  def method_missing(n, *a) = n == :to_str ? "abc" : super
  def ==(o) = o == "abc"
end
p ["abc" == StrLike.new, "abc" != StrLike.new, "x" == StrLike.new]

# `super` in method_missing reached through an explicit receiver is a
# NoMethodError, even right after a vcall miss of the same name went through it
class MM
  def method_missing(n, *a) = @hit ? super : (@hit = true; :mm)
  def go
    nope
    begin
      self.nope
    rescue NameError => e
      [e.class, e.message[/\A[^']*/]]
    end
  end
end
p MM.new.go

# IO#pos on a pipe is ESPIPE
r, w = IO.pipe
begin
  r.pos
rescue => ex
  p ex.class
end
r.close; w.close

# the message of raise(Cls, :sym) is the Symbol's name
class SymErr < StandardError; end
begin
  raise SymErr, :boom
rescue => ex
  p [ex.message, ex.inspect, ex.full_message(highlight: false).lines.first.sub(/\A.*?: /, "")]
end
print SymErr.new(:x), "\n"
puts SymErr.new(:y)
