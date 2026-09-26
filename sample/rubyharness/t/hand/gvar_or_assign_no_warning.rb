# `$x ||= v` reads $x without the -w "not initialized" warning (coderay's
# `$CODERAY_DEBUG ||= false` tripped rspec's no-warnings check); `$x &&=` and a
# plain read still warn.
def Warning.warn(m, **) = puts("warned: #{m[/\$\w+/]}")
$VERBOSE = true
$gw_a ||= 1
$gw_b &&= 1
p $gw_c
p $gw_a, $gw_b
$VERBOSE = false
