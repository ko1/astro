# return semantics: eval's top-level return (to the lambda / method of its binding), lambda by call (send(:lambda), redefined #lambda), define_method bodies, eval's bare super (2026-10-16)
def m1; eval("return 1"); 2; end
p m1
def m2; [1].each { eval("return 3") }; 4; end
p m2
def m3; pr = proc { eval("return 5") }; pr.call; 6; end
p m3
x = -> { proc { eval("return :lam") }.call; :after }.call
p x
y = lambda { eval("return :l2"); :no }.call
p y
def m4; -> { eval("return 7") }.call; 8; end
p m4
def t1; send(:lambda) { return }.call; :reached; end
p t1
class K; def lambda(&b) = b; def ret; lambda { return 1 }.call; 2; end; end
p K.new.lambda { 42 }.class, K.new.ret
def t2; l = lambda { return 3 }; [l.call, :after]; end
p t2
def t3; l = lambda { [1].each { return 4 }; 5 }; [l.call, :after]; end
p t3
l = lambda { return 6 }; p l.call
def t4; -> { return 7 }.call; 8; end
p t4
def mk = lambda { return 9 }
p mk.call
p [1,2].map(&lambda { |x| return x * 2 })
def t5; pr = proc { return 10 }; lambda { pr.call }.call; 11; end
p t5
class TT
  lamb = proc { |x| x.call }
  define_method :foo, lamb
  def mp(&b) = b
  def outer; pr = mp { return :good }; foo(pr); :bad; end
  define_method(:d1) { return 1; 2 }
  define_method(:d2) { [1].each { return 3 }; 4 }
  define_method(:d3) { |&b| b.call; :no }
  def outer2; d3 { return :out }; :bad2; end
  define_method(:d4) { break 5 }
end
t = TT.new
p t.outer, t.d1, t.d2, t.outer2, t.d4
class PP; def foo(a, b: 0, &blk) = [a, b, blk&.call]; end
class CC < PP
  def foo(a, b:, &block) = eval("super(a, b: b, &block)")
end
p CC.new.foo(1, b: 2) { 3 }
class DD < PP
  def foo(a, b:, &block) = eval("super")
end
p (DD.new.foo(1, b: 2) { 3 } rescue $!)
