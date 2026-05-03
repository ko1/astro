def f0(n) = f1(n)
def f1(n) = f2(n)
def f2(n) = f3(n)
def f3(n) = f4(n)
def f4(n) = f5(n)
def f5(n) = f6(n)
def f6(n) = f7(n)
def f7(n) = f8(n)
def f8(n) = f9(n)
def f9(n) = f10(n)
def f10(n) = f11(n)
def f11(n) = f12(n)
def f12(n) = f13(n)
def f13(n) = f14(n)
def f14(n) = f15(n)
def f15(n) = f16(n)
def f16(n) = f17(n)
def f17(n) = f18(n)
def f18(n) = f19(n)
def f19(n) = n

i=0
while i<50_000_000
  f0(42)
  i += 1
end
p f0(42)
