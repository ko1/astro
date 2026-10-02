# once a class's instances get ivars, Class#new hands the next ones an empty values array
# up front; a fresh object must still look ivar-less, and growing past it must work
class P
  def initialize(n) = n.times { |i| instance_variable_set(:"@v#{i}", i) }
end
3.times do |round|
  objs = [0, 1, 4, 5, 9].map { P.new(_1) }
  p objs.map { _1.instance_variables.size }, objs.last.instance_variable_get(:@v8)
end
fresh = P.new(0)
p fresh.instance_variables, fresh.instance_variable_defined?(:@v0), fresh.dup.instance_variables
p Marshal.load(Marshal.dump(fresh)).instance_variables, Marshal.load(Marshal.dump(P.new(6))).instance_variables.size
# two shapes from one class through the same initialize site
class Q
  def initialize(flag)
    if flag then @a = 1; @b = 2 else @b = 3 end
    @c = 4
  end
end
3.times { p [Q.new(true), Q.new(false)].map { [_1.instance_variables, _1.instance_variable_get(:@c)] } }
# allocate (no initialize) and a frozen fresh object
a = P.allocate
p a.instance_variables
f = P.new(0).freeze
begin
  f.instance_variable_set(:@x, 1)
rescue FrozenError => e
  p e.class
end
# many objects (GC moves them while their arrays are still empty)
arr = Array.new(20000) { |i| P.new(i % 3) }
p arr.sum { _1.instance_variables.size }, GC.start.nil?, arr[-1].instance_variables
