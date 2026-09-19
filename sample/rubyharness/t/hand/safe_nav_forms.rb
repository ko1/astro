# `recv&.m` の nil ガードは、演算子名の明示呼び出し・splat・... 転送でも効く。
# rack の Rack::Request が `...&.[](token)` を使うのでこれを踏む。
h = nil
p h&.[]("k")
p h&.[]=("k", 1)
g = { "k" => 7 }
p g&.[]("k")
p g&.[]=("z", 9)
p g
p g&.fetch("k")
a = nil
p a&.+(1)
p a&.-@
p a&.!
p a&.<<("x")
p a&.[](0)
b = 5
p b&.+(1)
p b&.-@
p b&.!
# splat / forwarding
arr = nil
args = [1, 2]
p arr&.push(*args)
arr2 = [0]
p arr2&.push(*args)
def fwd(x, ...) = x&.push(...)
p fwd(nil, 1, 2)
p fwd([9], 1, 2)
# ブロック付き
p arr&.map { |x| x }
p arr2&.map { |x| x * 2 }
# 連鎖
p nil&.foo&.bar
p({ a: { b: 1 } }&.[](:a)&.[](:b))
