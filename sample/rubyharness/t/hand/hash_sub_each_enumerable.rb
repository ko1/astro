# A Hash subclass that overrides #each: the Enumerable methods CRuby's Hash does
# not define itself (map, find, sort, min_by, ...) go through that #each; Hash's
# own (select, to_a, any?, include?, to_h) do not.  (REXML::Attributes.)
class H < Hash
  def each
    return to_enum(__method__) unless block_given?
    super { |k, v| yield [k, "v=#{v}"] }
  end
end
h = H.new; h[:a] = 1; h[:b] = 2
p h.collect { |k, v| [k, v] }, h.map { _1 }, h.select { |k, v| true }, h.to_a, h.sort, h.min_by { |k, v| v }
p h.each_with_index.to_a, h.find { |k, v| k == :a }, h.count, h.sort_by { |k, v| v }, h.inject([]) { |a, kv| a << kv }
p h.first, h.flat_map { |k, v| [k] }, h.group_by { |k, v| k }, h.each_with_object([]) { |kv, a| a << kv }, h.entries, h.zip([1,2])
p h.include?(:a), h.any? { |k, v| v == "v=1" }, h.to_h, h.filter_map { |k, v| v }, h.sum([]) { |k, v| [v] }, h.min
class H2 < Hash; end
g = H2[a: 1]; p g.map { |k, v| [k, v] }, g.sort, g.class.instance_method(:each).owner
k = {b: 2}; def k.each; yield [:x, :y]; end
p k.map { |a, b| [a, b] }, k.select { true }
p H.new.map { 1 }, H.instance_method(:map).owner == H.instance_method(:map).owner
