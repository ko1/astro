# Side-table data (builtin-subclass class, generic @ivars, singleton classes,
# #object_id) must survive GC for live objects while dead ones come and go.
class MyH < Hash; def tag = :myh; end
class MyS < String; end
m = Module.new { def ext = :ext }
keep_h = Array.new(50) { |i| MyH[i, i] }
keep_s = Array.new(50) { |i| MyS.new("s#{i}") }
keep_iv = Array.new(50) { |i| s = +"iv#{i}"; s.instance_variable_set(:@n, i); s }
keep_ext = Array.new(50) { |i| [i].extend(m) }
keep_sing = Array.new(50) { |i| o = Object.new; o.define_singleton_method(:who) { i }; o }
ids = keep_iv.map(&:object_id)
20.times do
  3000.times { |i| MyH.new; MyS.new("t"); (+"z").instance_variable_set(:@q, i); [].extend(m); Object.new.object_id }
  GC.start
end
p keep_h.all? { |h| h.is_a?(MyH) && h.tag == :myh }, keep_h[7][7]
p keep_s.all? { |s| s.class == MyS }, keep_s[3]
p keep_iv.each_with_index.all? { |s, i| s.instance_variable_get(:@n) == i }, keep_iv[5].instance_variables
p keep_ext.all? { |a| a.ext == :ext && a.singleton_class.include?(m) }
p keep_sing.each_with_index.all? { |o, i| o.who == i }
p keep_iv.map(&:object_id) == ids
