# syntax (hand): method definition forms
def square(x) = x * x
p square(5)

def endless_noargs = 42
p endless_noargs

class Calc
  def add(a, b) = a + b
  def -(o) = "minus#{o}"
  def [](i) = i * 10
  def []=(i, v); @last = [i, v]; @last; end
  def name=(n); @name = n; end
  def name; @name; end
  def positive?; true; end
  def danger!; :done; end
  def <=>(o); 0; end
end
c = Calc.new
p c.add(2, 3)
p(c - 1)
p c[5]
c[2] = 9
p c.instance_variable_get(:@last)
c.name = "x"
p c.name
p c.positive?
p c.danger!
p(c <=> c)

def with_default(a, b = a * 2)
  [a, b]
end
p with_default(3)
p with_default(3, 10)

def after_splat(a, *b, c)
  [a, b, c]
end
p after_splat(1, 2, 3, 4)
p after_splat(1, 2)

def kw_only(x:, y: 5)
  [x, y]
end
p kw_only(x: 1)
p kw_only(x: 1, y: 2)

def returns_multiple
  return 1, 2, 3
end
p returns_multiple

def implicit_last
  10
  20
  30
end
p implicit_last

def yields_twice
  yield 1
  yield 2
  :done
end
acc = []
r = yields_twice { |x| acc << x }
p [acc, r]

def use_block(&b)
  b.call(10)
end
p use_block { |n| n + 1 }

def forward_all(...)
  inner(...)
end
def inner(a, b, c)
  a + b + c
end
p forward_all(1, 2, 3)

def conditional_def
  if true
    :yes
  else
    :no
  end
end
p conditional_def
