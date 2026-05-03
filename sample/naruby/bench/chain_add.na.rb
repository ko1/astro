# Each step adds 1, so f0(0) = 10 after 10 chained calls.
# PGSD+LTO should fold constant addition into a single +10.
def f0(n) = f1(n + 1)
def f1(n) = f2(n + 1)
def f2(n) = f3(n + 1)
def f3(n) = f4(n + 1)
def f4(n) = f5(n + 1)
def f5(n) = f6(n + 1)
def f6(n) = f7(n + 1)
def f7(n) = f8(n + 1)
def f8(n) = f9(n + 1)
def f9(n) = n + 1

i=0
while i<10_000_000
  f0(0)
  i += 1
end
p f0(0)
