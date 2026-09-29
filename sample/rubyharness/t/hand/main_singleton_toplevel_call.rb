# Top-level calls while main has a singleton class (to_s / inspect, as CRuby): a top-level def
# runs through the cached global path, and a singleton method / extend on main still shadows it.
# Run under CRuby and koruby and diff.
def fib(n) = n < 2 ? n : fib(n - 1) + fib(n - 2)
3.times { p fib(10) }
p fib(12)
def self.fib(n) = :singleton
p fib(10)
p [to_s, inspect, self.to_s]
module M; def mm = :mm; end
include M
p mm
def kw(*a) = a.last
ruby2_keywords(:kw)
p Hash.ruby2_keywords_hash?(kw(a: 1))
def g = :top
p g
extend(Module.new { def g = :extended })
p g
def h = :h1
p h
class << self; def h = :h2; end
p h
p singleton_methods.sort
