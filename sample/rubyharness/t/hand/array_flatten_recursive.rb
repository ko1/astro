# Array#flatten on a recursive array raises ArgumentError (no SEGV). vs ruby.
p [1, [2, [3, [4]]]].flatten
p [1, [2, [3]]].flatten(1)
p [1, [2, [3]]].flatten(2)
p [[1, 2], [3, 4]].flatten
p [].flatten
a = [1, 2]; a << a
begin; a.flatten; rescue => e; p e.class; end
b = []; cc = [b]; b << cc
begin; b.flatten; rescue => e; p e.class; end
d = [1, [2, 3]]; ee = [d]; d << ee
begin; d.flatten; rescue => e; p e.class; end
