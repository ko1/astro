def whoami; __method__; end
p whoami
def outer; [1].map { __method__ }.first; end
p outer
p __method__
class C; def foo; __method__; end; def bar; __callee__; end; end
p C.new.foo
p C.new.bar
def with_args(a, b); __method__; end
p with_args(1, 2)
def recur(n); n == 0 ? __method__ : recur(n - 1); end
p recur(3)
def nested_blocks; [1].each { [2].each { return __method__ } }; end
p nested_blocks
def deep; [1].map { [2].map { [3].map { __method__ } } }; end
p deep.flatten
[1].each { p __method__ }
def logger; "[#{__method__}] msg"; end
p logger
def m1; m2; end
def m2; __method__; end
p m1
