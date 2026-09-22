# `def m(a, ...)` keeps `a` as a named parameter, so a bare `super` has to pass
# it AND the forwarded rest — not just the rest.
module Base
  def greet(a, *rest, **kw, &b) = ["B#greet", a, rest, kw, b&.call]
  def one(a) = ["B#one", a]
  def two(a, b) = ["B#two", a, b]
  def kwonly(a, k:) = ["B#kwonly", a, k]
  def greet_explicit(a, *rest, **kw, &b) = ["B#ge", a, rest, kw, b&.call]
end

class Sub
  include Base
  def greet(msg, ...) = super
  def one(msg, ...) = super
  def two(x, y, ...) = super
  def kwonly(a, ...) = super
  def greet_explicit(msg, ...) = super(msg, ...)
end

s = Sub.new
p s.greet("x")
p s.greet("x", 1, 2, k: 3)
p s.one("y")
p s.two(1, 2)
p s.kwonly("a", k: 9)
p s.greet("b") { 42 }
p s.greet_explicit("c", 5, k: 6) { 7 }

# a plain `def m(...)` (no leading params) still forwards everything
class Sub2
  include Base
  def greet(...) = super
end
p Sub2.new.greet("z", 1, k: 2) { 5 }

# forwarding to a plain method call, not super
def target(a, *r, **k, &b) = ["t", a, r, k, b&.call]
def relay(a, ...) = target(a, ...)
p relay(1, 2, k: 3) { 4 }

# `super` written inside a block still forwards the method's params
class Sub3
  include Base
  def greet(msg, ...) = [1].map { super }.first
end
p Sub3.new.greet("q", 8, k: 1)
