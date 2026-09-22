# `"lit".freeze` IS the fstring-pool entry (CRuby's opt_str_freeze), so every
# occurrence of the same literal is one object and `-it` gives that object back.
# A string frozen any OTHER way is never adopted into the pool: `-it` hands out
# the pool's own entry instead.
p [("zq_a".freeze).equal?("zq_a".freeze),
   ("zq_a".freeze).equal?(-"zq_a"),
   (-"zq_b").equal?(-"zq_b"),
   ("zq_a".freeze).frozen?]

a = "zq_c".freeze
p [(-a).equal?(a), a.frozen?, a.equal?("zq_c".freeze)]

p [("zq_d".dup.freeze).then { |s| (-s).equal?(s) },
   String.new("zq_e").freeze.then { |s| (-s).equal?(s) },
   (+"zq_f").freeze.then { |s| (-s).equal?(s) }]

# dedup: a dynamically built frozen string resolves to the literal's entry
d = %w(zq g h).join(" ").freeze
p [d.equal?("zq g h".freeze), (-d).equal?("zq g h".freeze), (-d).equal?((-"zq g h").freeze)]

# the pool is per byte-sequence, not per shape
p [(-"zq_i").equal?(-"zq_i".dup), (-"zq_i").equal?(-"zq_j"), (-"zq_i") == "zq_i"]

# `.freeze` on a non-literal receiver is an ordinary call (no interning)
e = "zq_k"
p [e.freeze.equal?(e), (-e).equal?(e), e.frozen?]

# a receiver with ivars or a subclass is never interned
s = "zq_l".dup
s.instance_variable_set(:@x, 1)
s.freeze
p [(-s).equal?(s), (-s).frozen?]
class MyStr < String; end
m = MyStr.new("zq_m").freeze
p [(-m).equal?(m), (-m).frozen?, (-m).class]

# interpolation is not a static literal, so `.freeze` stays a plain call
n = 1
p ["zq_#{n}".freeze.frozen?, "zq_#{n}".freeze.equal?("zq_#{n}".freeze)]
