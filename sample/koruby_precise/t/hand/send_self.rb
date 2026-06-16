class C
  def foo(x); x * 2; end
  def bar(a, b); a + b; end
  def run1; send(:foo, 5); end
  def run2; __send__(:bar, 3, 4); end
  def run3; d = [:foo, 9]; send(*d); end
  def run4; args = [:bar, 10, 20]; send(*args); end
end
c = C.new
p c.run1
p c.run2
p c.run3
p c.run4
p c.send(:foo, 7)
