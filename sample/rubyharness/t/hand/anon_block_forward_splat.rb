# bare `&` (anonymous block forward) と splat/受け手の組み合わせ
def sink(*a, **k, &b) = [a, k, b ? b.call : nil]
class R
  def sink(*a, **k, &b) = [a, k, b ? b.call : nil]
end
def f1(*args, &) = sink(*args, &)
def f2(*args, &) = R.new.sink(*args, &)
def f3(r, *args, &) = r.sink(*args, &)
def f4(*args, &) = [].push(*args, &).size
def f5(*args, **kw, &) = R.new.sink(*args, **kw, &)
p f1(1, 2) { :b }
p f2(1, 2) { :b }
p f3(R.new, 1, 2) { :b }
p f4(1, 2)
p f5(1, k: 2) { :b }
p f2(1, 2)
