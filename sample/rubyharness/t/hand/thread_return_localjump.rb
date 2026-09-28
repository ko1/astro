t = Thread.new { begin; return; rescue LocalJumpError => e; [e.class, e.message]; end }
p t.value
def m = Thread.new { begin; return 1; rescue LocalJumpError => e; e.class; end }.value
p m
def f = [1, 2].each { |x| return x }
p f
def g = proc { return 7 }.call
p g
