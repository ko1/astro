class C; def foo; 1; end; def <=>(o); 9; end; end
c = C.new
def c.foo; 2; end
def c.<=>(o); 0; end
p c.foo
p c.send(:foo)
p c.send(:<=>, 5)
