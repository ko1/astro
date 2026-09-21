# `super(*args) do ... end` into a parent whose method came from define_method:
# the literal block has to reach the body's `&blk`, and the body binds its args
# method-shaped (not block-shaped).  rspec-core defines `it` with define_method
# and its specs redefine it with `def self.it ... super(*args) { }`.
class A
  define_singleton_method(:it) do |*all_args, &block|
    [:A_it, all_args, block ? block.call : :noblk]
  end
  define_method(:go) do |a, b = 2, *rest, &blk|
    [:A_go, a, b, rest, blk ? blk.call : :noblk, block_given?]
  end
end
class B < A
  def self.it(*args, &block)
    args << {}
    super(*args) do
      [:inner, block.call]
    end
  end
  def go(*args, &block)
    super(*args) { [:inner_go, block ? block.call : nil] }
  end
end
p B.it(1) { :outer }
p B.new.go(1) { :outer_go }
p B.new.go(1, 9, 8, 7)

# bare `super` forwards the caller's own block
class C < A
  def go(a, &block)
    super
  end
end
p C.new.go(5) { :c_block }

# a define_method'd parent reached through a module extended in
module Ext
  def self.extended(k) = k.singleton_class.send(:define_method, :mk) { |*a, &b| [:ext, a, b ? b.call : nil] }
end
class D; extend Ext; end
class E < D
  def self.mk(*a, &b) = super(*a) { [:e, b.call] }
end
p E.mk(1) { :ez }

# arity is enforced method-shaped through super too
class F < A
  def go(*args, &block) = super(*args, &block)
end
begin
  F.new.go
rescue ArgumentError => e
  p e.message
end

# a literal `&:sym` reifies as Symbol#to_proc (a lambda of arity -2), not as an
# anonymous 0-arity block -- rspec's `match(&:tall?)` branches on `arity.zero?`
def grab(&b) = [b.class, b.arity, b.lambda?, b.parameters]
p grab(&:upcase)
def pass_through(&b) = b
pr = pass_through(&:upcase)
p [pr.arity, pr.call("ab"), pr.lambda?]
p pass_through(&:upcase).inspect.sub(/0x\h+/, "0xX")

# ... and `super` into a define_method whose body IS such a proc
class G
  define_method(:shout, &:upcase)
end
class H < G
  def shout(*a) = [:h, super(*a)]
end
p H.new.shout("hi")
class I0
  define_method(:call_it, &"xy".method(:sub))
end
class J0 < I0
  def call_it(*a) = [:j, super(*a)]
end
p J0.new.call_it("x", "Z")
