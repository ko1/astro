# `m(*a, &b)` / `recv.m(*a, &b)` / `super(*a, &b)`: the splatted Array is passed itself (CRuby's splatarray false),
# so a #to_proc that changes it is seen by the call; a `&(...)` expression evaluated after it gets a copy (splatarray
# true); non-Arrays convert as `*x` does.
def m(*x, **kw, &b) = [x, kw, b ? b.call : :noblk]
class P; def initialize(*x, &b) = (@x = x; @b = b && b.call); def v = [@x, @b]; end
class C < P; def initialize(*args, &b) = super(*args, &b); end
class R; def m(*x, &b) = [x, b ? b.call : :noblk]; end
r = R.new
grow = Object.new; def grow.to_proc = ($a << 3; proc { :grew })
shrink = Object.new; def shrink.to_proc = ($a.clear; proc { :shrank })
pr = proc { :pr }
[[:grow, grow], [:shrink, shrink], [:pr, pr], [:nil, nil]].each do |tag, blk|
  $a = [1, 2]; p [tag, :call, m(*$a, &blk)]
  $a = [1, 2]; p [tag, :send, r.m(*$a, &blk)]
  $a = [1, 2]; p [tag, :super, C.new(*$a, &blk).v]
end
$a = [1, 2]; p m(*$a, &($a << 4; pr))
$a = [1, 2]; p r.m(*$a, &($a.pop; pr))
p m(*nil, &pr), m(*(1..3), &pr), m(*[], &pr), m(*"s", &pr)
o = Object.new; def o.to_a = [:ta]; p m(*o, &pr)
h = ruby2_keywords_hash = Hash.ruby2_keywords_hash({ k: 1 })
p m(*[1, h], &pr)
p C.new(*[5, 6], &nil).v
p((r.m(*[7], &:to_s) rescue $!.class))
