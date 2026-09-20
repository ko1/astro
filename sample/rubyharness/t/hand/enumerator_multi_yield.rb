# 複数値を yield する Enumerator は arity を保つ: 1 引数ブロックには
# パックされた配列ではなく「最初の値」が渡る (CRuby は source を
# 再駆動するので自然にそうなる。koruby は materialize するので印を持たせる)。
a = %w[x y]
h = {a: 1, b: 2}
s = Struct.new(:p, :q).new(1, 2)
p a.each_with_index.map { |v| v }
p a.each_with_index.map { |v, i| [v, i] }
p a.each_with_index.to_a
p a.each_with_index.select { |v| v == "x" }
p a.each_with_index.first
p a.each_with_object([]).map { |v| v }
p a.each_with_object([]).to_a
p h.each.map { |v| v }
p h.each_pair.map { |v| v }
p h.each_with_index.map { |v| v }
p s.each_pair.map { |v| v }
en = Enumerator.new { |y| y.yield(1, 2); y.yield(3, 4) }
p en.map { |v| v }
p en.map { |v, w| [v, w] }
p en.to_a
p en.first
p a.each.map { |v| v }
p a.map.map { |v| v }
p 3.times.map { |v| v }
p a.each_with_index.each_with_index.map { |v| v }
# gather する側 (select/reject/take_while/drop_while/group_by/sort_by) と
# spread する側 (map/filter_map/flat_map/each) を取り違えない
e = a.each_with_index
p e.select { |v| v == "x" }
p e.reject { |v| v == "x" }
p e.filter_map { |v| v }
p e.take_while { |v| true }
p e.drop_while { |v| false }
p e.find { |v| v == "x" }
p e.group_by { |v| v }
p e.sort_by { |v| v.to_s }
p e.count
p e.each_slice(1).to_a
p e.with_index.map { |v| v }
p e.with_object(:o).map { |v| v }
