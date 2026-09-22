# The default definee ("cref") for a `def`, and what a Binding remembers.

# 1. `def` written inside a BLOCK belongs to the lexically enclosing class, not
#    to the top-level function table (pry's Pry::Method#super walked `owner`
#    and got Object).
class A
  define_method(:go) { def m1; end }
end
A.new.go; p A.instance_methods(false).sort

class B
  def go2; self.class.class_eval { def m2; end }; end
end
B.new.go2; p B.instance_methods(false).sort

class C1
  P = proc { def m3; end }
  define_method(:go3, &P)
end
C1.new.go3; p C1.instance_methods(false).sort

class D1
  def go4; [1].each { def m4; end }; end
end
D1.new.go4; p D1.instance_methods(false).sort

class E1
  def self.make = proc { def m5; end }
end
class F1; end
F1.new.instance_eval(&E1.make)          # the block's cref is E1, not F1
p [E1.instance_methods(false), F1.instance_methods(false)]

class G1
  def mk; G1.new.method(:mk); end
end
m = G1.new.mk
p [m.owner, m.name, m.owner.instance_method(m.name).class]

# 2. Binding#source_location works for the method form too (`Kernel.binding`
#    and `send(:binding)` reach the C builtin, which has no call node).
def ba; Kernel.binding; end
def bb; binding; end
def bc; Kernel.send(:binding); end
p [ba.source_location, bb.source_location, bc.source_location]

# 3. A binding taken in a method that was itself defined from an eval'd String
#    carries the cref that eval installed — the only thing separating
#    `obj.instance_eval("def m; binding; end")` (cref = obj's singleton) from
#    `TOPLEVEL_BINDING.eval("def self.m; binding; end")` (cref = Object).
o = Object.new
o.instance_eval("def __pry__; binding; end", "g", 1)
eval("def hello3; end", o.__pry__)
p o.methods(false).sort

main = TOPLEVEL_BINDING.eval("self")
main.instance_eval("def __tl__; binding; end", "g", 1)
eval("class DogFromEval; end", main.__tl__)
p [Object.const_defined?(:DogFromEval, false), main.singleton_class.const_defined?(:DogFromEval, false)]

# `class_eval(String)` names the cref, so `eval("def ...", b)` lands on it.
h = Class.new
eval("def hello4; end", h.class_eval("binding", "f", 1))
p h.instance_methods(false)

# 4. Module#ruby2_keywords is private (rspec-mocks gates on
#    `Module.private_method_defined?(:ruby2_keywords)`), and a flagged method
#    reports the anonymous `**` in #parameters.
p [Module.private_method_defined?(:ruby2_keywords), Module.method_defined?(:ruby2_keywords)]
class R2
  def fwd(a, *args, &blk); [a, args]; end
  ruby2_keywords :fwd
  def dots(...) = nil
end
p R2.instance_method(:fwd).parameters
p R2.instance_method(:dots).parameters
begin; Module.new.ruby2_keywords(:x); rescue NoMethodError => e; p e.class; end
