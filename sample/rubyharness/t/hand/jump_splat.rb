# return/next/break with splat arguments (CRuby wraps into an Array)
def a; return *[1,2]; end
def b; return *[1]; end
def c; return *[]; end
def d; return *5; end
def e; x=[9]; return *x, 10; end
p a, b, c, d, e
p [1,2,3].map { |x| next *[x] if x.odd?; x }
p [1,2,3].each { |x| break *[x, :done] if x == 2 }
def f; [1].each { |x| return *[x, :nonlocal] }; end
p f
