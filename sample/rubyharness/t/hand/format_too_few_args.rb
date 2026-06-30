begin; "%d %d" % [1]; rescue ArgumentError => e; p e.message; end
begin; "%d" % []; rescue ArgumentError => e; p e.class; end
begin; "%s %s %s" % ["a", "b"]; rescue ArgumentError => e; p e.class; end
p "%d %d" % [1, 2]
p "%d" % 5
p "%s" % "x"
p "%%"
p "%1$d %1$d" % [5]
p "%d %s" % [1, "a"]
p "no specifiers"
p "%d" % [42]
p format("%d-%d-%d", 1, 2, 3)
begin; format("%d %d %d", 1, 2); rescue ArgumentError => e; p e.class; end
