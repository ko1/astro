# `-x` through node_neg: frozen-string dedup of literals and mutable strings, numerics,
# a user -@, and String#-@ redefined later.  Run under both and diff.
a = -"lit"; b = -(+"lit"); s = +"x"; d = -s
p a.frozen?, a.equal?(b), d.frozen?, s.frozen?, (-s).equal?(d), -3, -(2**70), -1.5, -Rational(1, 2), -Complex(1, 2)
class Q; def -@ = :neg; end
p(-Q.new)
class String; def -@ = :redef; end
p(-"y")
