# Ruby permits repeated `_`-prefixed parameters in methods, blocks, and procs;
# they bind positionally and #parameters reports each. vs ruby.
def m(_, _); 42; end
p m(1, 2)
p method(:m).parameters
def m2(a, _, _); a; end
p m2(1, 2, 3)
p method(:m2).parameters
def m3(_, x, _); x; end
p m3(1, 5, 3)
pr = proc { |_, _| :ok }
p pr.call(1, 2)
p pr.parameters
lam = ->(_, _) { :l }
p lam.call(1, 2)
def mo(_, _ = 9); :o; end   # repeated with optional
p mo(1)
