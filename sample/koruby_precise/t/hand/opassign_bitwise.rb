x = 12; x &= 10; p x
y = 12; y |= 3; p y
z = 5; z ^= 1; p z
w = 1; w <<= 4; p w
v = 256; v >>= 2; p v
s = "a"; s <<= "bc"; p s
class C
  def initialize; @flags = 0xFF; @sh = 1; end
  def run; @flags &= 0x0F; @flags |= 0x80; @sh <<= 3; [@flags, @sh]; end
end
p C.new.run
