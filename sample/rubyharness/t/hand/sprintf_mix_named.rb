# String#% / format: named vs numbered vs unnumbered mixing errors (CRuby's
# check_*_arg), and the surplus-argument warning: $VERBOSE only, placed at the
# caller (file:line) and routed through Warning.warn so a program can filter it.
$stderr = $stdout
p("a %s" % ["x", "y"])
$VERBOSE = true; p("a %s" % ["x", "y"]); $VERBOSE = false
$DEBUG = true; begin; p("a %s" % ["x", "y"]); rescue => e; p e; end; $DEBUG = false
["%{a} %s", "%s %{a}", "%<a>s %s", "%{a} %1$s", "%1$s %s", "%s %1$s", "%1$s %{a}"].each do |f|
  begin; p(format(f, {a: 1})); rescue => e; p [f, e.class, e.message]; end
  begin; p(format(f, 5, 6)); rescue => e; p [f, e.class, e.message]; end
end
seen = []
Warning.singleton_class.prepend(Module.new { define_method(:warn) { |m, **| seen << m; nil } })
$VERBOSE = true
x = "a %s" % [1, 2]
y = format("b %s", 1, 2)
z = "c %s".%([1, 2])
$VERBOSE = false
p seen.size, seen.map { |m| m.sub(/\A.*\//, "") }
