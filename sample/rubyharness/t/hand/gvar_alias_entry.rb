$b = "base"
alias $prog $0
p $prog == $0
alias $0 $b
p $0
alias $0 $prog
p $0 == __FILE__
alias $out $stdout
p $out.equal?($stdout)
alias $m $~
"ab" =~ /b/
p $m
alias $x $y
$x = 1
p $y
$y = 2
p $x
