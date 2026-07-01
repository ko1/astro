# Enumerator#size: materialized-source enumerators report their length; a
# block generator with no given size returns nil (no crash). vs ruby.
p [1, 2, 3].each.size
p [1, 2, 3, 4, 5].map.size
p (1..10).each.size
p "abc".each_char.size
p [1, 2, 3].each_with_index.size
p Enumerator.new { |y| y << 1; y << 2 }.size
p [10, 20].each.with_index.size
