# case/when through node_caseeq's cache: class / module / Range / Regexp / Proc / user #=== when-values,
# a class's own self.===, extend, a subclass, and redefining Module#=== / Range#=== afterwards.
module Tag; end
class Base; end
class Kid < Base; include Tag; end
class Odd
  def self.===(x) = x.is_a?(Integer) && x.odd?
end
class Even < Base; end
module Ext
  def ===(x) = x == :ext
end
Even.extend(Ext)
class Matcher
  def initialize(v) = @v = v
  def ===(x) = x.to_s == @v
end
def kind(x)
  case x
  when Odd then :odd
  when Integer then :int
  when String, Symbol then :strsym
  when Tag then :tag
  when Base then :base
  when 1.0..2.0 then :range
  when /zz/ then :re
  when ->(v) { v.nil? } then :nilproc
  when Matcher.new("[1]") then :matcher
  when Even then :even_ext
  else :other
  end
end
xs = [3, 4, "s", :y, Kid.new, Base.new, 1.5, 9.5, [1], nil, Even.new, Object.new]
3.times { p xs.map { kind(_1) } }
p kind(:ext)
class Module
  alias_method :orig_eqq, :===
  def ===(x) = x == :forced || orig_eqq(x)
end
p xs.map { kind(_1) }, kind(:forced)
class Range
  def ===(x) = x == :range_forced
end
p kind(1.5), kind(:range_forced)
