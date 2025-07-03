p 0

__END__

i=0
while i<1_000_000_000
  i += 1
end

__END__
i=0
while i<3
  # p i
  i += 1
end

__END__
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

i=0
while i<100_000_000/2
  f0(42)
  i += 1
end

__END__
def f0(n) = f1(n)
def f1(n) = f2(n)
def f2(n) = 0

__END__


def f0(n) = f1(n)
def f1(n) = f2(n)
def f2(n) = 0

__END__

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

i=0
while i<1000_000
  f0(42)
  i += 1
end

__END__

def rec(n) = n > 0 ? rec(n-1) : 0
rec 1_000

__END__

def fib n
  if n < 2
    1
  else
    fib(n-2) + fib(n-1)
  end
end

fib 40

__END__


def f
  42
end

p f

__END__

p f

__END__

def test
  i=0
  while i<1_000_000_000
    i = bf_add(i, 1)
  end
end

test

__END__

def f = g
def g = 0
p f
p f
p f

__END__

def prime_count(limit)
  count = 0
  n = 2
  while n < limit
    i = 2

    while i * i <= n && n % i != 0
      i += 1
    end
    
    # i += 1 while i * i <= n && n % i != 0

    count += 1 if i * i > n
    n += 1
  end
  count
end

i = 0
while i<100
  n = 100_000
  prime_count(n)
  i+=1
end

__END__
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
def f19(n) = f20(n)
def f20(n) = f21(n)
def f21(n) = f22(n)
def f22(n) = f23(n)
def f23(n) = f24(n)
def f24(n) = f25(n)
def f25(n) = f26(n)
def f26(n) = f27(n)
def f27(n) = f28(n)
def f28(n) = f29(n)
def f29(n) = f30(n)
def f30(n) = f31(n)
def f31(n) = f32(n)
def f32(n) = f33(n)
def f33(n) = f34(n)
def f34(n) = f35(n)
def f35(n) = f36(n)
def f36(n) = f37(n)
def f37(n) = f38(n)
def f38(n) = f39(n)
def f39(n) = f40(n)
def f40(n) = f41(n)
def f41(n) = f42(n)
def f42(n) = f43(n)
def f43(n) = f44(n)
def f44(n) = f45(n)
def f45(n) = f46(n)
def f46(n) = f47(n)
def f47(n) = f48(n)
def f48(n) = f49(n)
def f49(n) = f50(n)
def f50(n) = f51(n)
def f51(n) = f52(n)
def f52(n) = f53(n)
def f53(n) = f54(n)
def f54(n) = f55(n)
def f55(n) = f56(n)
def f56(n) = f57(n)
def f57(n) = f58(n)
def f58(n) = f59(n)
def f59(n) = f60(n)
def f60(n) = f61(n)
def f61(n) = f62(n)
def f62(n) = f63(n)
def f63(n) = f64(n)
def f64(n) = f65(n)
def f65(n) = f66(n)
def f66(n) = f67(n)
def f67(n) = f68(n)
def f68(n) = f69(n)
def f69(n) = f70(n)
def f70(n) = f71(n)
def f71(n) = f72(n)
def f72(n) = f73(n)
def f73(n) = f74(n)
def f74(n) = f75(n)
def f75(n) = f76(n)
def f76(n) = f77(n)
def f77(n) = f78(n)
def f78(n) = f79(n)
def f79(n) = f80(n)
def f80(n) = f81(n)
def f81(n) = f82(n)
def f82(n) = f83(n)
def f83(n) = f84(n)
def f84(n) = f85(n)
def f85(n) = f86(n)
def f86(n) = f87(n)
def f87(n) = f88(n)
def f88(n) = f89(n)
def f89(n) = f90(n)
def f90(n) = f91(n)
def f91(n) = f92(n)
def f92(n) = f93(n)
def f93(n) = f94(n)
def f94(n) = f95(n)
def f95(n) = f96(n)
def f96(n) = f97(n)
def f97(n) = f98(n)
def f98(n) = f99(n)
def f99(n) = f100(n)
def f100(n) = n

i=0
while i<1000
  f0(42)
  i += 1
end

__END__
def fib n
  if n < 2
    1
  else
    fib(n-2) + fib(n-1)
  end
end

fib 35

__END__
def sum p1, p2, p3
  p1 + p2 + p3
end

def run
  sum(1, 2, 3)
end

run

__END__
def fib n
  if n < 2
    1
  else
    fib(n-2) + fib(n-1)
  end
end

fib 5


__END__

def f(a, b) = g(a, b)
def g(a, b) = h(a, b)
def h(a, b) = a + b

i=0
while i<10
  f(42, 43)
  i+=1
end

__END__

def f0(n)
  i=0
  while i<100_000
    i+=1
  end
end

i=0
while i<1_000
  f0(42)
  i += 1
end

__END__

def f0(n)
  i=0
  while i<100
    i+=1
  end
end

i=0
while i<1_000_000
  f0(42)
  i += 1
end

__END__

def f(n) = g(n)
def g(n) = n


f(42)

__END__
i=0
while i<10_000
  f(42)
  i+=1
end

__END__

__END__

def f(n)
  if n > 1
    f(n-1)
  else
    0
  end
end

f 5

__END__

def fib n
  if n < 2
    1
  else
    fib(n-2) + fib(n-1)
  end
end

fib 5

__END__
def 

def f(n) = g(n)
def g(n) = h(n)
def h(n) = n

i=0
while i<10
  f(42)
  i += 1
end

__END__



def tak x, y, z
  if y >= x
    z
  else
    tak( tak(x-1, y, z),
         tak(y-1, z, x),
         tak(z-1, x, y))
  end
end

p tak(18, 9, 0)


__END__

def fib n
  if n < 2
    1
  else
    fib(n-2) + fib(n-1)
  end
end

fib 30



__END__
def test
  i = 0
  while i < 100_000
    i += 1
  end
end

i=0
while i<1_000
  test
  i+=1
end

p i

__END__

def fib(n) = n + 42;
p fib(30)

__END__

def test
  i = 0
  while i < 1_000_000
    i += 1
  end
end

i=0
while i<1_000
  test
  i+=1
end

