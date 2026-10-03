# super / method lookup through deep include chains while GCs move classes (the cached MRO is forwarded, not dropped)
mods = (0...30).map { |i| Module.new { define_method(:chain) { [i] + (defined?(super) ? super() : []) } } }
base = Class.new { def chain = [:base] }
klasses = (0...8).map { |k| c = Class.new(base); mods.each_with_index { |m, i| c.include(m) if (i + k) % 3 != 0 }; c }
r = []
20.times do |t|
  klasses.each { |c| r << c.new.chain.sum { _1.is_a?(Integer) ? _1 : 1000 } }
  GC.start if t % 3 == 0
  Array.new(2000) { "x" * 10 }
end
p r.uniq
late = Module.new { def chain = [:late] + super }
klasses[0].include(late)
GC.start
p klasses[0].new.chain.first(3), klasses[0].ancestors.index(late)
klasses[1].prepend(Module.new { def chain = [:pre] + super })
GC.start
p klasses[1].new.chain.first(3)
