# compare_by_identity Hash / Set: an address index for 16+ keys that the GC drops when it
# visits the hash (keys may have moved) and lookups rebuild; switching a big value-indexed
# Hash to compare_by_identity no longer finds an equal-but-different String.
h = {}; 20.times { |i| h["k#{i}"] = i }; h.compare_by_identity; k = "k3"; p h[k], h.key?("k3"), h.size
objs = Array.new(300) { Object.new }
ih = {}.compare_by_identity
objs.each_with_index { |o, i| ih[o] = i }
garbage = []; 3000.times { |i| garbage << "x#{i}" * 3 }   # allocate: the GC may move the keys
p objs.each_with_index.all? { |o, i| ih[o] == i }, ih.size, ih[Object.new], ih.key?(objs[150])
ih.delete(objs[10]); p ih[objs[10]], ih[objs[11]], ih.size, objs.count { |o| ih.key?(o) }
s1 = "a"; s2 = "a".dup; sh = {}.compare_by_identity; 40.times { |i| sh["s#{i}"] = i }; sh[s1] = 1; p sh[s1], sh[s2], sh["s5"]
set = Set.new.compare_by_identity; objs.each { |o| set << o }; GC.start; p objs.all? { |o| set.include?(o) }, set.include?(Object.new), set.size
st = Set.new.compare_by_identity.replace(objs[0, 50]); p objs[0, 50].all? { |o| st.include?(o) }
d = ih.dup; GC.start; p objs.all? { |o| d.key?(o) }, d.compare_by_identity?
ih2 = {}.compare_by_identity; [1, :a, nil, 2.5, 3**70].each_with_index { |k, i| ih2[k] = i }; 20.times { |i| ih2[i + 100] = i }; p ih2[1], ih2[:a], ih2[nil], ih2[3**70], ih2[2.5], ih2[100]
h3 = {}; objs.each { |o| h3[o] = 1 }; h3.compare_by_identity; p h3[objs[7]], h3.size
