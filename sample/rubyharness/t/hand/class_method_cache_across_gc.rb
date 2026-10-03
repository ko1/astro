# resolved methods kept per class across GCs: redefinition, undef, include, remove_method and anonymous classes
module Greet; def hi = :greet; end
class Base; def hi = :base; def bye = :bye; end
class Kid < Base; end
k = Kid.new
r = []
4.times do |i|
  r << k.hi << k.bye
  GC.start
  Array.new(3000) { "x" * 8 }
end
p r.uniq
class Kid; include Greet; end
GC.start
p k.hi
class Kid; def hi = [:kid, super]; end
GC.start
p k.hi
class Kid; remove_method :hi; end
GC.start
p k.hi
class Base; undef_method :bye; end
GC.start
p((k.bye rescue :nomethod))
anon = (0...20).map { |i| Class.new(Kid) { define_method(:n) { i } } }
5.times { GC.start; Array.new(2000) { [] } }
p anon.map { _1.new.n }.sum, anon.map { _1.new.hi }.uniq
anon = nil
5.times { GC.start }
p Kid.new.hi
