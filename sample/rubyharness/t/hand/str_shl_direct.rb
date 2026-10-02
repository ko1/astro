# `str << str` appends in place in node_shl; everything String#<< does must still hold
out = +""
5.times { |i| out << "a" << i.to_s << "-" }
p out, out.encoding

# frozen receiver: FrozenError, message and the line of the `<<`
begin
  f = "frozen".freeze
  f << "x"
rescue FrozenError => e
  p e.message, e.backtrace_locations.first.lineno, e.backtrace_locations.first.label
end

# encoding negotiation and its error
u = +"café"
b = (+"\xff").force_encoding("BINARY")
a = +"ascii"
a << b
p a.encoding, a.bytesize
begin
  u << b
rescue Encoding::CompatibilityError => e
  p e.class, e.message
end
e1 = (+"").force_encoding("US-ASCII")
e1 << "x"
p e1.encoding
e1 << "é"
p e1.encoding

# self-append and non-String operands still go through String#<<
s = +"ab"
s << s
p s
s << 99
p s
class ToStr; def to_str = "TS"; end
s << ToStr.new
p s

# a subclass and a String with a singleton class keep their own #<<
class MyS < String
  def <<(x) = super("[#{x}]")
end
m = MyS.new("m")
m << "y"
p m
g = +"g"
def g.<<(x) = super(x.upcase)
g << "z"
p g

# refinement of String#<< applies only where it is used
module LoudShl
  refine String do
    def <<(x) = super(x.upcase)
  end
end
def plain_append(t) = t << "q"
using LoudShl
r = +"r"
r << "w"
p r, plain_append(+"p")

# a chilled literal (no magic comment) is appended to with its rhs intact
def chilled_append
  c = "chill"
  c << "ed" << "!"
  c
end
p chilled_append, chilled_append.frozen?
