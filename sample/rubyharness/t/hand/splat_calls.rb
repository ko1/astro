# splat in call / block / &proc / yield / super + anonymous rest (rubyspec follow-up)
def f(*a); yield a.sum; end
p f(*[1, 2, 3]) { |x| x * 10 }
p f(1, *[2, 3], 4) { |x| x }

def g(*a); a.map { |x| yield x }; end
pr = proc { |x| x + 100 }
p g(*[1, 2, 3], &pr)
p g(*["a", "bb", "ccc"], &:length)

def y(*a); yield(*a); end
y(*[1, 2, 3]) { |a, b, c| p [a, b, c] }
def yo; [[1, 2], [3, 4]].each { |pair| yield(*pair) }; end
yo { |a, b| p(a + b) }

class SBase
  def initialize(*l); @l = l; end
  def show; @l; end
  def add(a, b, c); a + b + c; end
end
class SSub < SBase
  def initialize(*l); super(*l); end
  def add(*args); super(*args); end
end
p SSub.new(1, 2, 3).show
p SSub.new.add(10, 20, 30)

def anon_rest(*); anon_target(*); end
def anon_target(a, b); a + b; end
p anon_rest(10, 20)

# receiver send with splat + block
p [10, 20, 30].each_with_object([]) { |x, acc| acc.push(*[x, x]) }
