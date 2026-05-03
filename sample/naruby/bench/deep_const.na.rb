# Deep constant chain: every call returns a constant.  Full PGSD
# inline + constant folding should eliminate everything.
def a = 1
def b = a
def c = b
def d = c
def e = d
def f = e
def g = f
def h = g
def i = h
def j = i

k=0
while k<100_000_000
  j
  k += 1
end
p j
