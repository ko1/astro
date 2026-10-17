# Differences CRuby's test/rubygems found (2026-10-17): ENV.replace with a nil value, format("%p") of an
# Array / Hash, Hash#sort / min / max / min_by with keys that have their own <=>, defined?(X) in a
# `module A::B` body (A is not in the nesting), dup of a singleton class, Marshal of a
# compare_by_identity Hash, Date's Marshal format
class V
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def inspect = "V#{n}"
end

ENV["RG_ZZ1"] = "a"
o = ENV.to_h
ENV.replace(o.merge("RG_ZZ2" => nil))
p [ENV["RG_ZZ2"], ENV.key?("RG_ZZ2"), ENV["RG_ZZ1"]]
ENV.replace(o)

puts format("%p|%s|%p|%-8s|", [V.new(1)], [V.new(2)], {a: V.new(3)}, [3])

h = {V.new(2) => 1, V.new(3) => 2, V.new(1) => 3}
p h.sort, h.min, h.max, h.min_by { _1 }, h.max_by { _1 }, h.minmax_by { _1 }
p((({1 => Object.new, 1.0 => 2}.min) rescue $!.class))

module Aa; X = 1; module Bb; end; end
module Aa::Bb; p [defined?(X), (X rescue :err)]; end
class Aa::Cc; p [defined?(X), (X rescue :err)]; def self.m = [defined?(X), (X rescue :err)]; end
p Aa::Cc.m
module Aa; module Bb; p defined?(X); end; end

sc = (class << Object.new; self; end)
p [(sc.dup rescue $!.class), (sc.clone rescue $!.message)]

cbi = Hash.new.compare_by_identity.tap { |x| x[+"a"] = 1; x[+"a"] = 2 }
cbd = Hash.new(0).compare_by_identity.tap { |x| x[+"a"] = 1; x[+"a"] = 2 }
[cbi, cbd].each { |x| e = Marshal.load(Marshal.dump(x)); p [e, e.compare_by_identity?, e.default] }

require "date"
[Date.new(1994, 12, 9), DateTime.new(2001, 2, 3, 4, 5, 6, "+7"), DateTime.new(2001, 2, 3, 23, 5, 6.5r, "-9"),
 Date.new(2000, 1, 1, Date::GREGORIAN)].each do |d|
  e = Marshal.load(Marshal.dump(d))
  p [Marshal.dump(d), e, e == d, e.start]
end
p Marshal.load("\x04\bU:\tDate[\vi\x00i\x03 a%i\x00i\x00i\x00f\f2299161".b)
