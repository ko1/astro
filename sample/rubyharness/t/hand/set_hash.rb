# Set#hash is content-based and order-independent (equal sets hash equal),
# so Sets work as Hash keys and in uniq. vs ruby.
require 'set'
p Set[1, 2, 3].hash == Set[3, 2, 1].hash
p Set[1, 2].hash == Set[1, 2].hash
p Set[1, 2].hash == Set[1, 2, 3].hash
h = { Set[1, 2] => "a", Set[3, 4] => "b" }
p h[Set[2, 1]]
p h[Set[4, 3]]
p [Set[1, 2], Set[2, 1], Set[1, 3]].uniq.size
p Set[Set[1, 2], Set[3, 4]].hash == Set[Set[4, 3], Set[2, 1]].hash
