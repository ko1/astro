# Symbol#clamp accepts the 1-arg Range form (and nil-bounded ranges). vs ruby.
p :m.clamp(:a..:z)
p :z.clamp(:a..:m)
p :a.clamp(:m..:z)
p :m.clamp(:a, :z)
p :m.clamp(..:d)
p :m.clamp(:p..)
begin; :m.clamp(:x); rescue => e; p e.class; end
