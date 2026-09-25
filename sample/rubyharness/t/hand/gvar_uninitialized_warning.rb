# $VERBOSE = true: reading a global that was never set warns (CRuby -w).
def Warning.warn(m, **) = puts(m.sub(/\A.*?warning: /, "warning: "))
p $undef1
$VERBOSE = true
x = $undef2
p defined?($undef3)
$set = 1; p $set
$VERBOSE = false
p $undef4
