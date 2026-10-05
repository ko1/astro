# Sends whose receiver is neither a plain builtin nor a plain user object (builtin subclasses, singleton-classed or
# extended objects, Thread) hit the site's instance cache directly: C and Ruby callees, and every way the answer
# can change after the cache is filled (redefinition, a later singleton method, extend, remove_method)
module Loud; def shout = "#{name}!"; end
class Named; attr_reader :name; def initialize(n) = @name = n; def shout = name; def frozen_name = name.frozen?; end
class HS < Hash; def tag = :hs; end
class AS < Array; end
a = Named.new("a"); b = Named.new("b"); def b.shout = "single #{name}"
c = Named.new("c"); c.extend(Loud)
hs = HS[k: 1]; as = AS[3, 4]
th = Thread.current
4.times do |i|
  GC.start if i == 2
  p [a, b, c].map(&:shout), [a, b, c].map { |o| o.shout }, b.frozen_name, b.frozen?, c.respond_to?(:shout)
  p hs.tag, hs.size, hs.key?(:k), hs.fetch(:k), as.size, as.first, as.sum, th.alive?, th[:q] = i, th.name.class
end
def a.shout = "late single"
p a.shout, [a, b].map(&:shout)
class Named; def shout = "redefined #{name}"; end
p [a, b, c].map(&:shout)
c.extend(Module.new { def shout = "ext2" })
p c.shout
class HS; remove_method :tag; end
begin
  hs.tag
rescue NoMethodError => e
  p e.class
end
class AS; def size = :own_size; end
p as.size, AS[1].size
