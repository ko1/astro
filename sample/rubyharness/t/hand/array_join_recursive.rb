# Array#join on a recursive array raises ArgumentError (no SEGV). vs ruby.
a = [1, 2]; a << a
begin; a.join("-"); rescue => e; p [e.class, e.message]; end
p [1, [2, [3, 4]], 5].join("-")
p [1, 2, 3].join
p [[1, 2], [3, 4]].join(",")
p [1, [2], 3].join("+")
b = []; cc = [b]; b << cc
begin; b.join; rescue => e; p e.class; end
p [].join("x")
p [1, nil, 2].join("-")
