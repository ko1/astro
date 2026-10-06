# String#dup: a bare String copies bytes + encoding (owner is String, as in CRuby); a subclass with
# #initialize_copy, generic ivars, a singleton method and a chilled / frozen literal take the full path
class SS < String
  def initialize_copy(o)
    super
    @x = 1
  end
end
2.times do |r|
  GC.start if r == 1
  s = "ab".encode("ASCII-8BIT"); d = s.dup
  p [d, d.encoding, d.frozen?, d.equal?(s), String.instance_method(:dup).owner]
  p [SS.new("z").dup.class, SS.new("z").dup.instance_variable_get(:@x)]
  t = +"q"; t.instance_variable_set(:@i, 2); p t.dup.instance_variable_get(:@i)
  u = +"w"
  def u.hi = 1
  p [u.dup.respond_to?(:hi), u.clone.respond_to?(:hi)]
  f = "lit".freeze; p [f.dup.frozen?, f.dup, "x#{r}".dup << "!", "é".dup.encoding, "é".dup.bytesize]
  big = "abc" * 100; bd = big.dup; bd << "z"; p [big.size, bd.size]
end
