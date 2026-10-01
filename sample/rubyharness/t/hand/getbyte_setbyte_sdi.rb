# str.getbyte(i) / str.setbyte(i, b) (an AOT build runs them in place, korb_sdi_tab): negative and
# out-of-range indexes, frozen receivers, coercion, bytes past 255, subclasses, singleton and
# later redefinition, non-String receivers, safe navigation, multibyte text.  Run under both and diff.
s = +"héllo"
p s.getbyte(0), s.getbyte(1), s.getbyte(-1), s.getbyte(99), s.getbyte(-99)
p s.setbyte(0, 72), s.setbyte(-1, 0x4f), s.setbyte(2, 300), s.bytes
begin; s.setbyte(99, 1); rescue => e; p e.class, e.message; end
p s.getbyte(1.9), s.setbyte(1.2, 65.7), s.bytes.first(3)
begin; s.getbyte("1"); rescue => e; p e.class; end
f = "frozen".freeze
begin; f.setbyte(0, 70); rescue => e; p e.class; end
p f.getbyte(0)
class MyStr < String
  def getbyte(i) = [:sub, super]
end
p MyStr.new("ab").getbyte(0)
t = +"xy"
def t.setbyte(i, b) = :singleton
p t.setbyte(0, 1), t
n = nil
p n&.getbyte(0)
begin; :sym.getbyte(0); rescue NoMethodError => e; p e.class; end
o = Object.new
def o.getbyte(i) = [:obj, i]
p o.getbyte(5)
buf = "\x00\x01\x02".b
3.times { |i| buf.setbyte(i, buf.getbyte(i) + 10) }
p buf.bytes
class String
  alias_method :orig_getbyte, :getbyte
  def getbyte(i) = orig_getbyte(i).to_s
end
p "AB".getbyte(0)
