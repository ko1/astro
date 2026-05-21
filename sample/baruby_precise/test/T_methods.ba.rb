# def / call / recursion / multiple args

def add1(x)
  x + 1
end
p add1(0)
p add1(99)

# 2 args
def mul(a, b)
  a * b
end
p mul(3, 4)
p mul(-2, 5)

# 3 args
def triadd(a, b, c)
  a + b + c
end
p triadd(1, 10, 100)

# recursion
def fact(n)
  if n <= 1
    1
  else
    n * fact(n - 1)
  end
end
p fact(5)
p fact(10)

# fib
def fib(n)
  if n < 2
    n
  else
    fib(n-1) + fib(n-2)
  end
end
p fib(10)
p fib(15)

# mutual recursion
def is_even(n)
  if n == 0
    true
  else
    is_odd(n - 1)
  end
end
def is_odd(n)
  if n == 0
    false
  else
    is_even(n - 1)
  end
end
p is_even(0)
p is_even(7)
p is_odd(7)

# def returns
def get_arr
  [10, 20, 30]
end
p get_arr
p get_arr.size

def get_str
  "hello"
end
p get_str

# nested call
def square(x)
  x * x
end
def sum_squares(a, b)
  square(a) + square(b)
end
p sum_squares(3, 4)
