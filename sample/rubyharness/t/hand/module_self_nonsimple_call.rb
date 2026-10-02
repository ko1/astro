# Implicit-self calls with a module / class / builtin as self reach non-simple
# methods (optional, keyword, rest, block params) and attr readers through the cache.
module Helpers
  class << self
    attr_accessor :count
  end
  self.count = 0

  def self.opt(a, b = 2) = [a, b]
  def self.kw(a, k: 1, **rest) = [a, k, rest]
  def self.rest(*xs) = xs.sum
  def self.blk(&b) = b ? b.call(3) : :noblk
  def self.post(a, *m, z) = [a, m, z]
  def self.run(i)
    self.count = count + 1
    [opt(i), opt(i, 9), kw(i), kw(i, k: 5, x: 1), rest(i, i, i), blk { _1 * i }, blk, post(1, 2, 3, i), count]
  end
  def self.priv_helper(x = 0) = x + 100
  private_class_method :priv_helper
  def self.call_priv(x) = priv_helper(x)
end
3.times { |i| p Helpers.run(i) }
p Helpers.call_priv(1)
begin
  Helpers.opt
rescue ArgumentError => e
  p e.message
end

class Klass
  def self.make(name, size: 3) = "#{name}:#{size}"
  def self.build(n) = (1..n).map { |i| make("k#{i}", size: i) } + [make("z")]
end
2.times { p Klass.build(2) }

class String
  def pad_to(w = 8, ch = ".") = ljust(w, ch)
  def padded = pad_to(6) + pad_to
end
p "ab".padded, "abc".padded

# redefinition between calls is seen
module Helpers
  def self.opt(a, b = 3) = [:new, a, b]
end
p Helpers.run(7).first(2)
