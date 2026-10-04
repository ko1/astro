# recv[i] / recv[i] = v on receivers other than plain Array/Hash/String go through the node's inline cache
class Box
  def initialize = @h = {}
  def [](k) = @h[k]
  def []=(k, v)
    @h[k] = v * 2
    :ignored
  end
end
class Hdr < Hash
  def [](k) = super(k.to_s.downcase)
  def []=(k, v)
    super(k.to_s.downcase, v)
  end
end
S = Struct.new(:a, :b)
class MM; def method_missing(n, *a) = n == :[] ? [:mm, a] : super; def respond_to_missing?(*) = true; end
b = Box.new; h = Hdr.new; st = S.new(1, 2); md = /(a)(b)?/.match("ab"); mm = MM.new
2.times do |i|
  x = (b[:k] = i)
  p x, b[:k], (h["Content-Type"] = i), h["CONTENT-TYPE"], h.keys, st[0], st[:b], (st[1] = 9), st.b, md[1], md[2]
  Thread.current[:tk] = i
  p Thread.current[:tk], mm[1, 2], mm[3]
end
class Box; def [](k) = [:redefined, k]; end
p b[:k]
def b.[](k) = :singleton
p b[:k], Box.new[:z]
class Raiser; def [](k) = raise(ArgumentError, "bad #{k}"); end
begin
  Raiser.new[:q]
rescue ArgumentError => e
  p e.message, e.backtrace[0][/in '.*'/]
end
fr = Box.new.freeze
begin
  fr[:a] = 1
rescue => e
  p e.class
end
3.times { |i| GC.start if i == 1; v = (b2 = Box.new)[:n] = "x" * 50; p v.size, b2[:n].size, h[:Q] = i }
st2 = S.new(1, 2)
begin
  st2[5]
rescue IndexError => e
  p e.message, e.backtrace_locations[0].lineno == __LINE__ - 2
end
