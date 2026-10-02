# String#<< / Array#<< from a prepended module beat node_shl's in-place append
module M; def <<(x) = super("!#{x}"); end
String.prepend(M)
s = +""
s << "a"
p s
module N; def <<(x) = super(x * 2); end
Array.prepend(N)
a = []
a << 1
p a
