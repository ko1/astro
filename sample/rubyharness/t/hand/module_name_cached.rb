# Module#name stays the same frozen String while nothing renames, and follows every renaming
module Outer; class Inner; end; end
a = Outer::Inner.name; b = Outer::Inner.name
p a, a.frozen?, a.equal?(b)
c = Class.new
p c.name
c.set_temporary_name("fake_name")
p c.name
Named = c
p c.name, c.name.equal?(c.name)
m = Module.new
n = Class.new
p n.name
m.const_set(:Deep, n)
p n.name.nil? || n.name.start_with?("#<Module")
Holder = m
p n.name, m.name
p String.name, Comparable.name, Outer.name.equal?(Outer.name)
3.times { |i| GC.start if i == 1; p Outer::Inner.name }
k = Class.new
p k.name
Later = k
p k.name, Later.name
