class M < Array
  def ===(o) = o == :hit
  def hash = 42
  def eql?(o) = o.is_a?(M) && o.first == first
end
a = M.new([1]); b = M.new([2]); c = M.new([1])
p((case :hit when a then :yes else :no end), a === :hit, [a].include?(:x))
h = {}; h[a] = 1
p h.key?(b), h.key?(c), h[c], {M.new([]) => 1}.key?([]), [a, b, c].uniq.size
class N < String; def hash = 1; def eql?(o) = true; end
p({N.new("x") => 1}.key?(N.new("y")))
