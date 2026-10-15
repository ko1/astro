require "objspace"
require "json"
# reachable_objects_from / memsize_of / dump: only what does not depend on addresses or exact sizes
p ObjectSpace.reachable_objects_from(Object.new)
p ObjectSpace.reachable_objects_from(%w[a b c]).to_set >= Set[Array, "a", "b", "c"]
p [nil, true, false, 42].map { |v| ObjectSpace.reachable_objects_from(v) }
removed = Object.new
kept = Object.new
ary = [removed, kept]
ary.shift
r = ObjectSpace.reachable_objects_from(ary)
p [r.include?(kept), r.include?(removed)]
q = Queue.new
q << kept
r = ObjectSpace.reachable_objects_from(q)
p (r + r.flat_map { |x| ObjectSpace.reachable_objects_from(x) || [] }).include?(kept)
p [nil, true, 42, :sym].map { |v| ObjectSpace.memsize_of(v) }
c = Class.new
before = ObjectSpace.memsize_of_all(c)
o = c.new
p ObjectSpace.memsize_of_all(c) > before
d = JSON.parse(ObjectSpace.dump("abc"))
p [d["type"], d["value"], d["bytesize"], d["encoding"]]
p ObjectSpace.dump_all(output: :string).include?('"value":"abc"')
begin
  ObjectSpace.dump("abc", output: Object.new)
rescue ArgumentError => e
  p e.message.start_with?("wrong output option")
end
