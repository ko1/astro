# A define_method'd method behaves like a lambda: it swallows its own `return`.
# But a `return` written in a block the CALLER passed in is aimed at the
# caller's method and has to keep unwinding through it (rspec's `RSpec.describe`
# is define_method'd, and specs `return` out of the group block they pass).
class C
  define_singleton_method(:run) { |&b| [:ran, b.call] }
  def self.exec_it(&b) = [:exec, Class.new.module_exec(&b)]
  define_singleton_method(:dm_exec) { |&b| C.exec_it(&b) }
  define_method(:each2) { |&b| [1, 2, 3].each { |i| b.call(i) } }
end

def outer1
  r = C.run { return :from_block }
  [:after, r]
end
p outer1

def outer2
  r = C.dm_exec { return :from_block2 }
  [:after2, r]
end
p outer2

def find_first
  C.new.each2 { |i| return i if i > 1 }
  :none
end
p find_first

# its own `return` is still a plain return from the method
class D
  define_method(:m) { return :dm_ret; :never }
  define_method(:nested) { [1, 2].each { |i| return [:early, i] if i == 2 }; :never }
  define_method(:lam, ->(x) { return x * 2 })
  define_method(:pr, proc { |x| return x + 1 })
end
p [D.new.m, D.new.nested, D.new.lam(3), D.new.pr(3)]

# ... even when the method that defined it is still running
def setup_and_call(k)
  k.send(:define_method, :m) { return :x }
  [:called, k.new.m]
end
p setup_and_call(Class.new)

# `break` in a block passed to a define_method'd method is still the call's value
def outer3
  r = C.run { break :broke }
  [:after3, r]
end
p outer3
