p(-> { r = 1; [_1, r] }.call(:a))
p(proc { r = 9; [_1, _2, r] }.call(:a, :b))
