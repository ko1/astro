# is_a? / Module#=== / case-when on plain instances, singletons, modules (included / extended); String#to_s /
# Array#to_a / Hash#to_h return self only for an exact builtin, a subclass instance converts
module Mx; end
class Ba; end; class Su < Ba; include Mx; end
class SS < String; end
o = Su.new; s = "x"; ss = SS.new("y")
p o.is_a?(Ba), o.is_a?(Mx), o.is_a?(String), Ba === o, Mx === o, String === s, String === ss, SS === s, BasicObject === o
p s.to_s.equal?(s), ss.to_s.class, ss.to_s.equal?(ss), [1].to_a.class, Class.new(Array).new([1]).to_a.class, {a: 1}.to_h.equal?({a: 1})
def o.extra = 1; p o.is_a?(Ba), Ba === o, o.nil?, o.class
x = Object.new; x.extend(Mx); p Mx === x, x.is_a?(Mx)
p (case o when String then :s when Mx then :mx end), (case 3 when Integer then :i end), nil.nil?, NilClass === nil
