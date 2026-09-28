# an anonymous rest followed by post parameters: `->(*, a)` / `|x, *, y|`
l = ->(*, a) { a }
p l.(1), l.(1, 2, 3), l.arity, l.parameters, (l.() rescue $!.class)
pr = proc { |x, *, y| [x, y] }
p pr.(1, 2, 3, 4), pr.([5, 6, 7]), pr.(1), pr.arity, pr.parameters
p [[1, 2, 3]].map { |a, *, b| [a, b] }
m = ->(a, b = 9, *, c, d) { [a, b, c, d] }
p m.(1, 2, 3), m.(1, 2, 3, 4, 5, 6)
