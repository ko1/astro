# obj.x = v (attr_writer) through the cached small entry, and String#encoding through the AOT
# inline send: frozen receivers, overrides and redefinitions must behave as CRuby
class Node
  attr_writer :start_line, :end_line
  attr_reader :start_line, :end_line
  def set(a, b) = (self.start_line = a; self.end_line = b; [start_line, end_line])
end
def assign(o, v) = (o.start_line = v)
n = Node.new
3.times { |i| p assign(n, i), n.start_line, n.set(i, i + 1) }
p assign(Node.new, :fresh)
f = Node.new.freeze
begin
  assign(f, 1)
rescue FrozenError => e
  p e.class, e.message.sub(/0x\h+/, "XX")
end
class SubNode < Node; def start_line=(v); @start_line = [:sub, v]; end; end
s = SubNode.new
p assign(s, 5), s.start_line
class Node; def start_line=(v); @start_line = [:redef, v]; end; end
p assign(n, 6), n.start_line
o = Node.new
def o.start_line=(v); @start_line = :singleton; end
p assign(o, 7), o.start_line
# String#encoding
strs = ["abc", "日本".encode("UTF-8"), "x".b, "y".encode("UTF-16LE"), "z".force_encoding("ASCII")]
def enc(x) = x.encoding
3.times { p strs.map { enc(_1).to_s } }
class MyS < String; end
p enc(MyS.new("q")).to_s, enc(:sym).to_s, enc(/re/).to_s
class String; alias __enc encoding; def encoding = :redef_enc; end
p enc("w")
class String; alias encoding __enc; end
p enc("w").to_s
