# def の分割代入パラメータ `def m(a, (b, c), d = 1)`。
# 分割パラメータは positional 1 個ぶんだが local は複数使うので、後続の
# パラメータの local がずれる。required 側だけ本体先頭で移していて、
# optional 側を移していなかった (= 「repeated parameter name」で弾いていた)。
# ruby/ruby の core_assertions.rb が `def assert_respond_to(obj, (meth, *priv), msg = nil)`。
def m(a, (b, c), d = 1)
  [a, b, c, d]
end
p m(1, [2, 3])
p m(1, [2, 3], 4)
def n((x, *y))
  [x, y]
end
p n([1,2,3])
def o(a, (b, (c, d)), e)
  [a,b,c,d,e]
end
p o(1, [2,[3,4]], 5)
def p2((a,b), (c,d))
  [a,b,c,d]
end
p p2([1,2],[3,4])
def m1(a, (b, c)); [a,b,c]; end
p m1(1,[2,3])
def m2((b, c)); [b,c]; end
p m2([2,3])
def m3((x, *y)); [x,y]; end
p m3([1,2,3])
def m4(a, (b,(c,d)), e); [a,b,c,d,e]; end
p m4(1,[2,[3,4]],5)
def m(a, (b, c), d = 1); [a,b,c,d]; end
p m(1,[2,3]); p m(1,[2,3],4)
def n(a, (b,*c), d = :x, *r, e, f: 9, &blk); [a,b,c,d,r,e,f,blk&.call]; end
p n(1,[2,3,4],5,6,7,8)
p n(1,[2],9)
def q(obj, (meth, *priv), msg = nil); [obj, meth, priv, msg]; end
p q(:o, :m)
p q(:o, [:m, true], "z")
def r2((a,b), c = 1, (d,e)); [a,b,c,d,e]; end
p r2([1,2],[3,4])
p r2([1,2],9,[3,4])
def s2(a, (b,c), *rest); [a,b,c,rest]; end
p s2(1,[2,3],4,5)
def t2(a, (b,c), k: 1, **kw); [a,b,c,k,kw]; end
p t2(1,[2,3],k: 5, z: 6)
