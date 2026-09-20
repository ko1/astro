# `&:sym` は Symbol#to_proc と同じ: 受け手の後ろの引数も渡り、auto-splat しない。
p [1, 2, 3].reduce(&:+)
p [1, 2, 3].inject(&:+)
p [{a: 1}, {b: 2}, {c: 3}].reduce(&:merge)
p [[1, 2], [3, 4]].map(&:first)
p [[1, 2], [3, 4]].map(&:last)
p [1, 2, 3].each_with_object([]) { |x, o| o << x }
def y2; yield(1, 2); end
p y2(&:+)
def y3; yield([1, 2]); end
p y3(&:first)
def y0; yield; end
begin
  y0(&:+)
rescue ArgumentError => e
  p e.message
end
h = {x: 1, y: 2}
p h.map(&:first)
def take(&b) = b
b = take(&:+)
p b.call(3, 4)
p [1, 2, 3].map(&:to_s)
p [[1, 2], [3, 4]].each_with_object([]) { |(a, b), o| o << a + b }
