# Hash keys whose #hash / #eql? are Kernel's (plain objects, classes) are found by identity without dispatch;
# a user-defined #hash / #eql? / == still decides; a singleton #hash on another class does not disturb them
class K; end
class C2; def hash = 1; def eql?(o) = o.is_a?(C2); end
class E; def ==(o) = true; end
k1 = K.new; k2 = K.new; h = { k1 => 1, K => :cls, C2.new => :c2, E.new => :e }
p h[k1], h[k2], h[K], h[C2.new], h[E.new], h.key?(K), h.size
class << C2; def hash = 3; end   # some singleton redefines #hash: class keys still behave
p h[K], h[k1], { C2 => 5 }[C2]
3.times { GC.start; p h[k1], h[K], h[k2] }
