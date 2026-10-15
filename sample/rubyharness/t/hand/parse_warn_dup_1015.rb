# parser warnings reach Warning.warn once each (prism and koruby both used to report some) (2026-10-15)
def Warning.warn(*a) = ($stdout.puts "W: #{a.inspect}")
$VERBOSE = true
eval "{ key: :value, key: :value2 }"
eval "def m(**) = 1; m(a: 1, a: 2)"
eval "def mm; END { }; end"
eval "defined?(x); 1"
$VERBOSE = false
eval "{ k: 1, k: 2 }"
eval "defined?(y); 1"
