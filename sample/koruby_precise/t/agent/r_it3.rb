p(proc { a = 5; [it, a] }.call(:x))
p(proc { a = it; [it, a] }.call(:x))
p(proc { q = 1; w = 2; [it, q, w] }.call(:x))
