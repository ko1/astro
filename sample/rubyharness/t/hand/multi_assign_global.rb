# multi-assign to global / mixed non-local targets. vs ruby.
$a, $b, $c = 1, 2, 3
p [$a, $b, $c]
class T
  def go
    x, $g, @i = 10, 20, 30
    [x, $g, @i]
  end
end
p T.new.go
p $g
$p, $q = [7, 8, 9]
p [$p, $q]
# swap via globals
$x, $y = 1, 2
$x, $y = $y, $x
p [$x, $y]
