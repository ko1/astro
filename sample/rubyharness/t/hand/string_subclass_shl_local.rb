# Operators a String / Array / Hash subclass defines itself are dispatched (not the builtin fast path).
class B < String; def |(v) = concat("[#{v}]"); def &(v) = concat("&#{v}"); def ^(v) = concat("^#{v}"); def <<(v) = concat(v.to_s); end
b = B.new("x"); b |= 2; b &= 3; b ^= 4; b << 5; p b
@o = B.new("i"); @o |= 7; p @o
c = B.new; x = (c | "a"); p x, x.class
class A2 < Array; def |(o) = :mine; end; p A2.new | [1]
class H2 < Hash; def <(o) = :lt; end; p H2.new < {}
b = B.new("y"); b << 5; b << 6; p b, b.class
