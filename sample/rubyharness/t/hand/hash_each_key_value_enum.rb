# Hash#each_key / #each_value without a block return an Enumerator over the
# keys / values (drivable via to_a/map/with_index/first). vs ruby.
h = {a: 1, b: 2, c: 3}
p h.each_key.to_a
p h.each_value.to_a
p h.each_key.class
p h.each_key.map { |k| k.to_s }
p h.each_value.select { |v| v > 1 }
p h.each_key.with_index.to_a
p h.each_value.reduce(:+)
p h.each_key.first(2)
p h.each_value.max
p({}.each_key.to_a)
# block form still returns self
p h.each_key { |k| }.equal?(h)
p h.each_value { |v| }.equal?(h)
