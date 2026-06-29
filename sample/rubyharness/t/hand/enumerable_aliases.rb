m = Enumerable.instance_method(:map)
[[:collect, :map], [:filter, :select], [:find_all, :select], [:detect, :find], [:entries, :to_a], [:member?, :include?], [:inject, :reduce], [:collect_concat, :flat_map]].each do |a, b|
  print "#{a}==#{b}: "
  p(Enumerable.instance_method(a) == Enumerable.instance_method(b))
end
class C; include Enumerable; def each; yield 1; yield 2; yield 3; end; end
c = C.new
p c.collect { |x| x * 2 }
p c.inject(:+)
p c.detect { |x| x > 1 }
p c.member?(2)
