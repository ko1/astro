class HasToInt; def to_int; 3; end; end
class BadToInt; def to_int; "x"; end; end
p(1 << HasToInt.new)
p(256 >> HasToInt.new)
p(begin; 1 << nil; rescue => e; [e.class.to_s, e.message]; end)
p(begin; 1 << "x"; rescue => e; e.class.to_s; end)
p(begin; 1 << BadToInt.new; rescue => e; [e.class.to_s, e.message]; end)
p((10 ** 30) << HasToInt.new)
