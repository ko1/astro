# A &b Proc's break is a LocalJumpError once the method that received the literal block
# has returned (called later, from another method, or yielded to); while that method is
# still running (forwarded to each / yield / recursion) the break ends its call.
def t = (yield rescue [$!.class, $!.message])
def get(&b) = b
pr = get { break :done }
p t { [1].each(&pr) }, t { pr.call }
def fwd(&b) = [1, 2, 3].each(&b)
p fwd { |x| break x * 10 if x == 2 }
def yielder = yield(5)
def fwd2(&b) = yielder(&b)
p fwd2 { |x| break x + 1 }
def outer(&b) = [1, 2].map { |i| inner(i, &b) }
def inner(i) = yield(i)
p outer { |i| break :early if i == 2; i }
class Foo; def each(&b) = @a.each(&b); def initialize = @a = [1, 2, 3]; end
p Foo.new.each { |x| break x if x == 2 }
def cap(&b) = b
def call_it(b) = b.call
p t { call_it(cap { break 1 }) }
def rec(n, &b) = n == 0 ? yield : rec(n - 1, &b)
p rec(3) { break :deep }
