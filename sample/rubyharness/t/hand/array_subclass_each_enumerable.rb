class R < Array
  def each = size.times { |i| yield(self[i].is_a?(Integer) ? self[i] * 10 : self[i]) }
end
r = R[1, 2, 3]
p r.each_with_index.to_a, r.each_slice(2).to_a, r.inject(:+), r.map { _1 }, r.select(&:odd?), r.group_by(&:odd?), r.min_by { -_1 }, r.to_a
r.each_with_index { |x, i| p [x, i] }
class R2 < Array; end
p R2[1, 2].each_with_index.to_a
