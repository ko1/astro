# Range#to_a/#each over a Symbol range (via succ); endless range -> RangeError. vs ruby.
p (:a..:e).to_a
p (:a...:e).to_a
p (:az..:bd).to_a
p (:a..:e).map { |s| s.to_s }
p (:x..:x).to_a
r = []; (:p..:t).each { |s| r << s }; p r
begin; (1..).to_a; rescue => e; p e.class; end
begin; (1...).to_a; rescue => e; p e.class; end
p (1..5).to_a
