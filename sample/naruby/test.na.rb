def f0(a) = a + 0
def f1(a) = a + 1
def f2(a) = a + 2
def f3(a) = a + 3
def f4(a) = a + 4
def f5(a) = a + 5
def f6(a) = a + 6
def f7(a) = a + 7
def f8(a) = a + 8
def f9(a) = a + 9
def f10(a) = a + 10
def f11(a) = a + 11
def f12(a) = a + 12
def f13(a) = a + 13
def f14(a) = a + 14
def f15(a) = a + 15
def f16(a) = a + 16
def f17(a) = a + 17
def f18(a) = a + 18
def f19(a) = a + 19
def f20(a) = a + 20
def f21(a) = a + 21
def f22(a) = a + 22
def f23(a) = a + 23
def f24(a) = a + 24
def f25(a) = a + 25
def f26(a) = a + 26
def f27(a) = a + 27
def f28(a) = a + 28
def f29(a) = a + 29
def f30(a) = a + 30
def f31(a) = a + 31
def f32(a) = a + 32
def f33(a) = a + 33
def f34(a) = a + 34
def f35(a) = a + 35
def f36(a) = a + 36
def f37(a) = a + 37
def f38(a) = a + 38
def f39(a) = a + 39
def f40(a) = a + 40
def f41(a) = a + 41
def f42(a) = a + 42
def f43(a) = a + 43
def f44(a) = a + 44
def f45(a) = a + 45
def f46(a) = a + 46
def f47(a) = a + 47
def f48(a) = a + 48
def f49(a) = a + 49
def f50(a) = a + 50
def f51(a) = a + 51
def f52(a) = a + 52
def f53(a) = a + 53
def f54(a) = a + 54
def f55(a) = a + 55
def f56(a) = a + 56
def f57(a) = a + 57
def f58(a) = a + 58
def f59(a) = a + 59
def f60(a) = a + 60
def f61(a) = a + 61
def f62(a) = a + 62
def f63(a) = a + 63
def f64(a) = a + 64
def f65(a) = a + 65
def f66(a) = a + 66
def f67(a) = a + 67
def f68(a) = a + 68
def f69(a) = a + 69
def f70(a) = a + 70
def f71(a) = a + 71
def f72(a) = a + 72
def f73(a) = a + 73
def f74(a) = a + 74
def f75(a) = a + 75
def f76(a) = a + 76
def f77(a) = a + 77
def f78(a) = a + 78
def f79(a) = a + 79
def f80(a) = a + 80
def f81(a) = a + 81
def f82(a) = a + 82
def f83(a) = a + 83
def f84(a) = a + 84
def f85(a) = a + 85
def f86(a) = a + 86
def f87(a) = a + 87
def f88(a) = a + 88
def f89(a) = a + 89
def f90(a) = a + 90
def f91(a) = a + 91
def f92(a) = a + 92
def f93(a) = a + 93
def f94(a) = a + 94
def f95(a) = a + 95
def f96(a) = a + 96
def f97(a) = a + 97
def f98(a) = a + 98
def f99(a) = a + 99

def f n
i=0
while i<n
  f0(i)
  f1(i)
  f2(i)
  f3(i)
  f4(i)
  f5(i)
  f6(i)
  f7(i)
  f8(i)
  f9(i)
  f10(i)
  f11(i)
  f12(i)
  f13(i)
  f14(i)
  f15(i)
  f16(i)
  f17(i)
  f18(i)
  f19(i)
  f20(i)
  f21(i)
  f22(i)
  f23(i)
  f24(i)
  f25(i)
  f26(i)
  f27(i)
  f28(i)
  f29(i)
  f30(i)
  f31(i)
  f32(i)
  f33(i)
  f34(i)
  f35(i)
  f36(i)
  f37(i)
  f38(i)
  f39(i)
  f40(i)
  f41(i)
  f42(i)
  f43(i)
  f44(i)
  f45(i)
  f46(i)
  f47(i)
  f48(i)
  f49(i)
  f50(i)
  f51(i)
  f52(i)
  f53(i)
  f54(i)
  f55(i)
  f56(i)
  f57(i)
  f58(i)
  f59(i)
  f60(i)
  f61(i)
  f62(i)
  f63(i)
  f64(i)
  f65(i)
  f66(i)
  f67(i)
  f68(i)
  f69(i)
  f70(i)
  f71(i)
  f72(i)
  f73(i)
  f74(i)
  f75(i)
  f76(i)
  f77(i)
  f78(i)
  f79(i)
  f80(i)
  f81(i)
  f82(i)
  f83(i)
  f84(i)
  f85(i)
  f86(i)
  f87(i)
  f88(i)
  f89(i)
  f90(i)
  f91(i)
  f92(i)
  f93(i)
  f94(i)
  f95(i)
  f96(i)
  f97(i)
  f98(i)
  f99(i)
  i += 1
end
end

n = 1_000
i = 0

while i<10_000
  i += 1
  f 1000
end


__END__

def prime?(n)
  if n < 2
    0
  else
    prime = 1

    i = 2
    while i * i <= n
      prime =  0 if n%i == 0
      i += 1
    end

    prime
  end
end

i = 0
while i < 1_000_000
  prime?(i)
  i += 1
end

__END__
i = 0
while 1
  i += 1
end

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

