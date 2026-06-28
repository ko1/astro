f = proc { |x| x + 1 }
g = lambda { |x| x * 2 }
p((f >> g).lambda?)
p((g >> f).lambda?)
p((f >> g).call(3))
p((g << f).call(3))
p((f << g).lambda?)
