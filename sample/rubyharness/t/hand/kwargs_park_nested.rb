# A keyword method with no **kwrest has no frame slot for its keyword Hash, so
# it parks it on the context.  An optional parameter's DEFAULT may call another
# keyword method, which parks its own -- the outer one has to survive that, and
# it cannot ride in a C local: the default runs Ruby and a moving GC relocates
# the Hash.
def churn(n) = Array.new(n) { |i| { :k => "v#{i}" * 4 } }

def inner(a:, b: 2)
  churn(1500)
  [a, b]
end

def outer(x: 1, y: inner(a: churn(1000).size), z: inner(a: 7, b: 8))
  [x, y, z]
end
p outer
p outer(x: 9)
p outer(x: 9, y: :given)

# three levels deep, each parking its own
def lvl3(p3: 3) = [p3]
def lvl2(p2: lvl3.first) = [p2]
def lvl1(p1: lvl2.first, q1: churn(800).size) = [p1, q1]
p lvl1
p lvl1(p1: 0)

# a default that both allocates and reads the caller's keywords
def mix(a:, b: a * 2, c: (churn(1200); a + b))
  { :a => a, :b => b, :c => c }
end
p mix(a: 5)
p mix(a: 5, b: 1)
p mix(a: 5, c: 0)
