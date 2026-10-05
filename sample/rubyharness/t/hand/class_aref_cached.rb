# Klass[k] / Klass[k] = v / `when Klass` through the operator node's cache with a class receiver: user def self.[],
# inherited ones, builtin constructors (Array[] / Hash[] / Set[] / Struct), redefinition, private, method_missing
class Reg
  @h = {}
  def self.[](k) = @h.fetch(k) { "none:#{k}" }
  def self.[]=(k, v)
    @h[k] = v
  end
  def self.===(o) = o.is_a?(Symbol)
end
class SubReg < Reg; @h = { z: 26 }; end
module Mod; def self.[](*a) = a.sum; end
S = Struct.new(:a, :b)
3.times do |i|
  GC.start if i == 1
  Reg[:a] = i
  p Reg[:a], Reg[:q], SubReg[:z], SubReg[:a], Mod[1, 2, i]
  p Array[1, i], Hash[[[:k, i]]], S[i, 2].to_a, (case :x when Reg then :sym else :no end), (case 1 when Reg then :sym else :no end)
  p [Reg, SubReg].map { |k| k[:z] }
end
class Reg; def self.[](k) = [:redefined, k]; end
p Reg[1], SubReg[1]
class << SubReg; def [](k) = :sub_own; end
p SubReg[1], Reg[2]
class Ghost; def self.method_missing(n, *a) = [n, a]; def self.respond_to_missing?(*) = true; end
p Ghost[1, 2]
class Boom; def self.[](k) = raise(KeyError, "no #{k}"); end
begin
  Boom[:x]
rescue KeyError => e
  p e.message, e.backtrace_locations.first.label
end
x = [Reg, Mod, Array]
p x.map { |k| k == Mod ? k[5] : k == Array ? k[5] : :skip }
4.times { p [Reg, String, Symbol, Reg].map { |k| case :x when k then 1 else 0 end } }
