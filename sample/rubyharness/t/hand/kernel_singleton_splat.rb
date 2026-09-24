# `Kernel.<global builtin>` is dispatched straight to the global function table,
# which is right until something is defined ON the Kernel module object itself.
# The fixed-arity call site checked for that; the splat one did not, so
# `Kernel.exec(env, *cmd)` ran the REAL exec even with a stub in place — an
# rspec suite that stubs Kernel.exec had its process replaced mid-run
# (simplecov's cli_spec stopped after 1335 of 1423 examples, silently, exit 0).
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 60]]
end

module MyMod; def self.go(*a) = [:real, a]; end
class MyCls;  def self.go(*a) = [:real, a]; end

# control: a user module / class honours an override with fixed and splat args
def MyMod.go(*a) = [:mine, a]
def MyCls.go(*a) = [:mine, a]
t(:mod_fixed) { MyMod.go(1, 2) }
t(:mod_splat) { a = [1, 2]; MyMod.go(*a) }
t(:cls_fixed) { MyCls.go(1, 2) }
t(:cls_splat) { a = [1, 2]; MyCls.go(*a) }

# the real thing: a singleton on Kernel must win on BOTH call shapes
t(:fmt_plain)  { Kernel.format("%d-%d", 1, 2) }
t(:fmt_splat0) { a = ["%d-%d", 1, 2]; Kernel.format(*a) }
def Kernel.format(*a) = [:mine, a]
t(:fmt_fixed)  { Kernel.format("%d", 7) }
t(:fmt_splat)  { a = ["%d", 7]; Kernel.format(*a) }
t(:fmt_mixed)  { a = [7]; Kernel.format("%d", *a) }
t(:fmt_send)   { a = ["%d", 7]; Kernel.send(:format, *a) }
t(:fmt_method) { a = ["%d", 7]; Kernel.method(:format).call(*a) }
class << Kernel; remove_method :format; end
t(:fmt_back)   { a = ["%d-%d", 3, 4]; [Kernel.format("%d", 5), Kernel.format(*a)] }

# a module_function-style caller reaches the same dispatch from another self
module Caller
  module_function
  def via_splat(*a) = Kernel.format(*a)
  def via_fixed(x, y) = Kernel.format(x, y)
end
def Kernel.format(*a) = [:mine2, a]
t(:caller_splat) { Caller.via_splat("%d", 9) }
t(:caller_fixed) { Caller.via_fixed("%d", 9) }
class << Kernel; remove_method :format; end
t(:caller_back)  { [Caller.via_splat("%d", 9), Caller.via_fixed("%d", 9)] }

# and the ordinary implicit-self call is untouched either way
t(:implicit)     { format("%d", 6) }
t(:kernel_puts)  { Kernel.respond_to?(:puts) }
