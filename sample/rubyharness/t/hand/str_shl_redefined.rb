# String#<< reopened globally: node_shl must stop appending in place
s = +"x"
s << "1"
p s
class String
  alias_method :orig_shl, :<<
  def <<(x) = orig_shl("<#{x}>")
end
s << "2"
p s
t = +""
3.times { |i| t << i.to_s }
p t
