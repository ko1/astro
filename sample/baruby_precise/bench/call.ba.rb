# 10-level pass-through chain.  acc accumulates so the result stays
# live (otherwise gcc/PGO could DCE the call).  Same shape as
# chain20.na.rb / chain40.na.rb at depth 10.
def f0(n) = f1(n)
def f1(n) = f2(n)
def f2(n) = f3(n)
def f3(n) = f4(n)
def f4(n) = f5(n)
def f5(n) = f6(n)
def f6(n) = f7(n)
def f7(n) = f8(n)
def f8(n) = f9(n)
def f9(n) = n

acc = 0
i = 0
while i < 100_000_000
  acc = acc + f0(42)
  i += 1
end
p acc
