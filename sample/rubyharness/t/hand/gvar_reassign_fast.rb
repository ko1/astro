# `$x = v` again takes the indexed fast path (no constant-table scan): the value, special gvars, a gvar set inside a
# method, and constant reassignment (which still warns and records its location) all behave as before.
$a = 1; $a = 2; p $a
def setg(v) = ($b = v)
setg(:x); setg(:y); p $b
$/ = "\n"; $/ = "\n"; p $/, $-0.equal?($/)
$, = nil; p $,
module M; X = 1; end
p M.const_source_location(:X).last
$stderr = $stdout
module M; X = 2; end
p M::X, M.const_source_location(:X).last
100.times { |i| $c = i }; p $c
p global_variables.include?(:$a), defined?($a)
