# C methods of builtin receivers (Proc/Range/Regexp/MatchData/Symbol/nil/true/false) called straight from the site cache keep CRuby's behavior
pr = proc { |a, b| [a, b] }; la = ->(x) { x * 2 }; r = (1..5); re = /b(.)/; md = re.match("abc"); s = :sym
2.times do
  p pr.call(1), pr.call(1, 2, 3), la.call(4), la.(5), pr.(6, 7), r.first, r.min, r.to_a.size, re.source, md[1], md.pre_match
  p s.size, :upcase.to_proc.call("ab"), nil.to_a, nil.inspect, true.to_s, false & true, s.length, r.cover?(3)
end
begin
  la.call(1, 2)
rescue ArgumentError => e
  p e.message
end
def raiser = proc { raise "in proc" }
begin
  raiser.call
rescue => e
  p e.message, e.backtrace.size > 0
end
p [1, 2].map(&:to_s), pr.call { 1 }
class Range; def first(*) = :patched; end
p r.first, (1..2).first
class Proc; alias __orig_call call; def call(*a) = [:patched_call, a]; end
p pr.call(9), la.call(1)
class Proc; alias call __orig_call; end
p pr.call(9), la.call(1)
module Shout; refine Symbol do; def size = 99; end; end
p s.size
using Shout
p s.size
module Loud; def inspect = "LOUD"; end
class NilClass; prepend Loud; end
p nil.inspect
3.times { |i| GC.start if i == 1; p la.call(i), md.post_match, re.match?("xbz") }
