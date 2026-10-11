# recv.send(:sym, args) / __send__ on an explicit receiver: the simple-method shortcut and every case that leaves it.
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.sub(/0x\h+/, "0xADDR")]
end
class A
  def pub(a, b) = [:pub, a, b]
  def zero = :zero
  def where = caller(1, 1)[0][/in '[^']*'/]
  def opt(a, b = 2) = [:opt, a, b]
  def kw(a, k: 1) = [:kw, a, k]
  def h(x) = x
  private def priv(x) = [:priv, x]
  protected def prot = :prot
end
class B < A; def zero = :b_zero; end
class MySend; def send(*a) = [:own_send, *a]; def zero = :zero; end
class MM; def method_missing(n, *a) = [:mm, n, *a]; def respond_to_missing?(*) = true; end
o = A.new; b = B.new
10.times do |i|
  t(:pub)       { o.send(:pub, i, 2) }
  t(:zero)      { o.send(:zero) }
  t(:sub)       { b.send(:zero) }
  t(:dunder)    { o.__send__(:pub, 1, i) }
  t(:priv)      { o.send(:priv, i) }
  t(:prot)      { o.send(:prot) }
  t(:arity)     { o.send(:pub, 1) }
  t(:arity2)    { o.send(:zero, 1) }
  t(:opt)       { o.send(:opt, i) }
  t(:kw)        { o.send(:kw, i, k: 3) }
  t(:hashpos)   { o.send(:h, k: i) }
  t(:str_name)  { o.send("zero") }
  t(:own_send)  { MySend.new.send(:zero, i) }
  t(:mm)        { MM.new.send(:nope, i) }
  t(:missing)   { o.send(:nope) }
  t(:where)     { o.send(:where) }
  t(:psend_priv){ o.public_send(:priv, 1) }
  t(:psend)     { o.public_send(:pub, 1, 2) }
end
s = A.new
def s.zero = :singleton
t(:singleton) { s.send(:zero) }
# (send under `using` does not see the refinement in koruby — a separate, older gap; not tested here)
