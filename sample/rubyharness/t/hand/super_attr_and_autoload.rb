# Two things bundler needs.
#
# 1. `super` from a method into an INHERITED attr_writer.  koruby's korb_super
#    handled attr_reader / builtin / cfunc / define_method but fell through to
#    the ISEQ path for a writer, whose `body` is NULL — a SEGV.
# 2. A module's autoload table must not show up in #instance_variables.  koruby
#    keeps it in a hidden `@__autoloads` ivar, and bundler's `Plugin.reset!`
#    does `instance_variables.each { |i| remove_instance_variable(i) }`, which
#    silently wiped every autoload the module had registered.
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 60]]
end

class A1; attr_writer :src; attr_reader :src; end
class B1 < A1; def src=(v); super; end; end
t(:attr_w_super) { o = B1.new; o.src = 7; o.src }

class A2; attr_reader :v; def initialize = (@v = 1); end
class B2 < A2; def v; super; end; end
t(:attr_r_super) { B2.new.v }

class A3; attr_accessor :w; end
class B3 < A3; def w=(x); super(x * 2); end; end
t(:attr_w_arg)   { o = B3.new; o.w = 3; o.w }

module M4; attr_writer :z; end
class C4; include M4; def z=(v); super; end; def z = @z; end
t(:attr_w_mod)   { o = C4.new; o.z = 5; o.z }

class A5; attr_writer :q; end
class B5 < A5; def q=(*a); super(*a); end; def q = @q; end
t(:attr_w_splat) { o = B5.new; o.q = 9; o.q }

class A6; attr_writer :f; end
class B6 < A6; def f=(v); super; end; end
t(:attr_w_frozen) { o = B6.new.freeze; (o.f = 1) rescue [$!.class, $!.message[0, 30]] }

# --- autoload bookkeeping is invisible ------------------------------------
module Au
  autoload :Nope, File.expand_path("no_such_autoload_target", __dir__)
  @kept = 1
end
t(:ivars_clean)  { Au.instance_variables }
t(:autoload_q)   { Au.autoload?(:Nope).is_a?(String) }
t(:reset_keeps)  { Au.instance_variables.each { |i| Au.remove_instance_variable(i) }
                   [Au.instance_variables, Au.autoload?(:Nope).is_a?(String)] }
t(:obj_ivars)    { o = Object.new; o.instance_variable_set(:@a, 1); o.instance_variables }
t(:class_ivars)  { k = Class.new; k.instance_variable_set(:@b, 2); k.instance_variables }
