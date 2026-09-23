# `recv&.m(args)` must not EVALUATE its arguments when recv is nil.  koruby's
# node_send_safe stages its children before it can look at the receiver, so the
# argument-bearing form needs the nil check first.  bundler's SpecSet does
# `@sorted&.insert(@sorted.bsearch_index { ... } || @sorted.size, spec)` — with
# the arguments evaluated eagerly that is a NoMethodError on nil.
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 60]]
end

t(:arg_skipped)  { n = 0; x = nil; [x&.foo(n += 1), n] }
t(:arg_run)      { n = 0; x = [1]; [x&.first(n += 1), n] }
t(:recv_on_nil)  { x = nil; x&.insert(x.size, 1) }
t(:two_args)     { n = 0; x = nil; [x&.foo(n += 1, n += 1), n] }
t(:kwargs)       { n = 0; x = nil; [x&.foo(k: (n += 1)), n] }
t(:kwsplat)      { n = 0; x = nil; h = {a: 1}; [x&.foo(**h, k: (n += 1)), n] }
t(:splat)        { n = 0; x = nil; [x&.foo(*[n += 1]), n] }
t(:block)        { n = 0; x = nil; [x&.map { n += 1 }, n] }
t(:block_arg)    { n = 0; x = nil; [x&.foo(n += 1) { n += 10 }, n] }
t(:chain)        { n = 0; x = nil; [x&.a(n += 1)&.b(n += 1), n] }
t(:setter)       { n = 0; x = nil; x&.foo = (n += 1); n }
t(:op_asgn)      { x = nil; x&.bar ||= 1 }
t(:receiver_once){ n = 0; a = [1, 2]; f = -> { n += 1; a }; [f.call&.first(1), n] }

# the receiver is evaluated exactly once, even when it IS nil
class Once
  def initialize = (@n = 0)
  attr_reader :n
  def val = (@n += 1; nil)
end
t(:once_nil)     { o = Once.new; r = o.val&.foo(1, 2); [r, o.n] }

# and the non-nil path still passes everything through
class Recv
  def m(*a, **k, &b) = [a, k, b&.call]
end
t(:full)         { Recv.new&.m(1, 2, k: 3) { :blk } }
t(:priv)         { k = Class.new { private def p1 = 1 }; o = k.new; (o&.p1 rescue $!.class) }
