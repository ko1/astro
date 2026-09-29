# Drives cext_api_test.so (api_test.c); the output must be identical under CRuby
# and koruby (run.sh diffs them).  `t` prints a value or the exception it raised.
$LOAD_PATH.unshift(ARGV[0] || File.join(__dir__, "build"))
require "cext_api_test"
T = CextAPITest

def t(label)
  r = yield
  puts "#{label}: #{r.inspect}"
rescue Exception => e
  puts "#{label}: raised #{e.class}: #{e.message}"
end

# immediates / numbers
t(:imm) { T.imm }
t(:num2long) { [T.num2long(3), T.num2long(-7), T.num2long(2.9), T.num2long(-2**62)] }
t(:num2long_str) { T.num2long("x") }
t(:num2long_nil) { T.num2long(nil) }
t(:num2long_big) { T.num2long(2**70) }
t(:num2long_to_int) { o = Object.new; def o.to_int = 11; T.num2long(o) }
t(:num2int) { T.num2int(2**31 - 1) }
t(:num2int_big) { T.num2int(2**40) }
t(:num2int_small) { T.num2int(-2**40) }
t(:num2dbl) { [T.num2dbl(1), T.num2dbl(0.25), T.num2dbl(2**64)] }
t(:num2dbl_str) { T.num2dbl("1.0") }
t(:num2ulong) { [T.num2ulong(5), T.num2ulong(2**64 - 1)] }
t(:bignums) { T.bignums }
t(:fix2long) { [T.fix2long(41), T.fix2long(2**70)] }

# types
t(:types) { [nil, true, false, 1, 2**70, 1.5, 1e300, :s, "s", [], {}, String, Kernel, Object.new, T::Counter.new, /r/].map { |v| T.type_of(v) } }
t(:check_string) { T.check_string("ok") }
t(:check_string_bad) { T.check_string(1) }
t(:check_string_nil) { T.check_string(nil) }
t(:type_p) { [T.type_p("s"), T.type_p([1]), T.type_p(2**80)] }

# strings
t(:str_new) { T.str_new }
t(:str_encodings) { T.str_encodings }
t(:str_upcase) { s = +"hello, World"; T.str_upcase!(s); s }
t(:str_upcase_frozen) { T.str_upcase!("frozen".freeze) }
t(:str_ptr_len) { T.str_ptr_len("bytes\0in") }
t(:string_value) { o = Object.new; def o.to_str = "conv"; [T.string_value("s"), T.string_value(o)] }
t(:string_value_bad) { T.string_value(3) }
t(:string_value_nil) { T.string_value(nil) }
t(:string_value_cstr_nul) { T.string_value_cstr("a\0b") }
t(:str_ops) { T.str_ops("abc", "abd") }
t(:str_set_len) { T.str_set_len }
t(:sprintf) { T.sprintf(:sym) }
t(:sprintf_str) { T.sprintf("q\"uote") }
t(:pin) { T.pin_across_gc("pinned bytes " * 3) }
t(:pin_fresh) { T.pin_across_gc(+"x" * 100) }

# arrays / hashes
t(:ary) { T.ary }
t(:check_ary) { o = Object.new; def o.to_ary = [:o]; [T.check_ary([1]), T.check_ary(nil), T.check_ary(o), T.check_ary(1)] }
t(:to_ary) { [T.to_ary(nil), T.to_ary(1), T.to_ary([2]), T.to_ary(1..3)] }
t(:hash) { T.hash({ a: 1, stop: 2, c: 3 }) }
t(:hash_default) { h = Hash.new(:dflt); h[:a] = 1; T.hash(h) }
t(:hash_foreach_delete) { h = { x: 1, del: 2, y: 3 }; T.hash(h); h }
t(:hash_fetch) { T.hash_fetch({ a: 1 }, :a) }
t(:hash_fetch_miss) { T.hash_fetch({ a: 1 }, :b) }
t(:hash_delete) { T.hash_delete({ a: 1, b: 2 }, :a) }
t(:check_hash) { o = Object.new; def o.to_hash = { o: 1 }; [T.check_hash({ a: 1 }), T.check_hash(nil), T.check_hash(o)] }

# calls / blocks
t(:funcall) { [T.funcall(41), T.funcall(1.5), T.funcall("a") ] rescue $!.class }
t(:funcall_str) { T.funcall("a") }
t(:funcallv) { [T.funcallv([3, 1, 2], :sort, []), T.funcallv("abc", :sub, ["b", "B"]), T.funcallv(Kernel, :format, ["%03d", 7])] }
t(:funcallv_private) { o = Object.new; def o.secret = :ok; o.singleton_class.send(:private, :secret); T.funcallv(o, :secret, []) }
class PrivK; private def secret = :ok; end
t(:funcall_public_private) { T.funcall_public(PrivK.new, :secret) }
t(:funcall_public) { T.funcall_public([1, 2], :size) }
t(:funcall_nomethod) { T.funcallv(1, :no_such_method_here, []) }
t(:block_given) { [T.block_given, T.block_given {}] }
t(:yield) { T.yield(3) { |x| x * 10 } }
t(:yield_values) { T.yield_values { |a, b, c| [c, b, a] } }
t(:yield_splat) { T.yield_splat([1, 2]) { |a, b| a + b } }
t(:yield_noblock) { T.yield(1) }
t(:block_proc) { T.block_proc { |x| x + 100 } }
t(:need_block) { T.need_block }
t(:passing_block) { T.passing_block([1, 2, 3]) { |x| x * x } }
t(:yield_each) { r = []; [T.yield_each(4) { |i| r << i }, r] }
t(:break_through_c) { T.yield_each(10) { |i| break i * 100 if i == 3 } }
t(:next_in_block) { r = []; T.yield_each(3) { |i| next if i == 1; r << i }; r }
t(:throw_through_c) { catch(:tag) { T.yield_each(10) { |i| throw :tag, i if i == 2 }; :not_thrown } }
t(:raise_through_c) { begin; T.yield_each(10) { |i| raise KeyError, "at #{i}" if i == 1 }; rescue KeyError => e; e.message; end }
t(:return_through_c) { def rtc = T.yield_each(5) { |i| return [:returned, i] if i == 2 }; rtc }

# exceptions
t(:raise) { T.raise(ArgumentError, "v") }
t(:raise_sym) { T.raise(TypeError, :sym) }
t(:raise_custom) { k = Class.new(StandardError) { def message = "custom:#{super}" }; T.raise(k, 1) }
t(:protect) { o = Object.new; def o.go = raise(IndexError, "from ruby"); T.protect(o) }
t(:protect_rethrow) { T.protect_rethrow }
t(:ensure_ok) { T.ensure(false) }
t(:ensure_raise) { T.ensure(true) }
t(:ensure_log) { T.ensure_log }
t(:rescue_std) { T.rescue(ArgumentError) }
t(:rescue_nonstd) { T.rescue(NotImplementedError) }
t(:rescue2_hit) { T.rescue2(KeyError) }
t(:rescue2_miss) { T.rescue2(ArgumentError) }
t(:exc_new) { e = T.exc_new; [e.class, e.message] }
t(:exc_raise) { T.exc_raise(RangeError.new("rr")) }
t(:sys_fail) { T.sys_fail }
t(:error_arity) { [0, 5].map { |n| begin; T.error_arity(n); rescue ArgumentError => e; e.message; end } }
t(:check_arity) { [T.check_arity(2), T.check_arity(9), (T.check_arity(1) rescue $!.message)] }
t(:frozen) { [T.frozen(+"x"), (T.frozen("y".freeze) rescue [$!.class, $!.message])] }
t(:protect_yield_ok) { T.protect_yield { |x| x + 1 } }
t(:protect_yield_raise) { T.protect_yield { raise Errno::EPIPE } }
t(:protect_yield_break) { T.protect_yield { break 77 } }

# classes / methods
t(:define) { T.define }
t(:methods) { o = T::Base.new; [o.m0, o.m1(1), o.m3(1, 2, 3), o.mv, o.mv(1, 2), o.ma(3, 4), o.m0_alias, T::Base.sing, T::Base::ANSWER] }
t(:attr) { o = T::Base.new; o.attr_x = 5; o.attr_x }
t(:mself) { o = T::Base.new; o.mself.equal?(o) }
t(:arity0) { T::Base.new.m0(1) }
t(:arity1) { T::Base.new.m1 }
t(:arity3) { T::Base.new.m3(1, 2) }
t(:private) { T::Base.new.mpriv }
t(:private_send) { T::Base.new.send(:mpriv) }
t(:mixin) { s = T::Sub.new; [s.from_mixin, s.m0, T::Sub.ancestors.take(3), T::Mixin.twice(21)] }
t(:mixin_private) { Object.new.extend(T::Mixin).twice(1) }
t(:method_arity) { [T::Base.instance_method(:m0).arity, T::Base.instance_method(:m3).arity, T::Base.instance_method(:mv).arity] }
t(:redefine_mismatch) { T.redefine_mismatch }
t(:define_not_class) { T.define_not_class }
t(:class_info) { T.class_info(T::Sub.new) }
t(:consts) { T.consts }
t(:path2class_bad) { T.path2class_bad }
t(:ivars) { o = Object.new; [T.ivars(o), o.instance_variables, o.instance_variable_get(:@b)] }
t(:class_ivar) { k = Class.new; [T.class_ivar(k), k.instance_variables] }
t(:subclass_in_ruby) { k = Class.new(T::Base) { def m0 = "ruby:" + super }; k.new.m0 }
t(:new_instance) { T.new_instance(T::Counter, 5).value }

# T_DATA
t(:counter) { c = T::Counter.new(3); c.incr.incr; [c.value, T.get_counter(c), T.data_p(c), T.data_p("s")] }
t(:counter_default) { T::Counter.new.value }
t(:counter_sub) { k = Class.new(T::Counter) { def initialize(x) = super(x * 2) }; k.new(4).value }
t(:counter_allocate) { T::Counter.allocate.value }
t(:counter_wrongtype) { T.get_counter("str") }
t(:counter_wrongtype2) { T.get_counter(Object.new) }
t(:dfree) { before = T.dfree_count; 2000.times { T::Counter.new }; GC.start; T.dfree_count > before }
t(:dmark_keeps) {
  c = T::Counter.new
  c.stash("stashed " + "string")
  5.times { GC.start; Array.new(1000) { "garbage" } }
  [c.stashed, c.stashed.equal?(c.stashed)]
}

# handles and GC
t(:identity) { T.identity(Object.new) }
t(:identity_str) { T.identity(+"str") }
t(:globals) {
  T.store_globals("registered " + "a", ["global", "b"])
  5.times { GC.start; Array.new(2000) { "x" * 10 } }
  T.load_globals
}
t(:gc_churn) { T.gc_churn(20000) }
t(:values_across_gc) { T.values_across_gc("s" * 20, [1, "two", :three], { 1 => "one" }) }
t(:nested) { T.nested(->(s) { T.nested(->(t) { s + "/" + t + "/" + T.yield(1) { |x| x.to_s } }) }) }
t(:deep_nesting) { f = ->(n) { n == 0 ? "bottom" : T.nested(->(s) { f.(n - 1) + "<" }) }; f.(30) }

# scan_args / kwargs
t(:scan_11) { [T.scan_11(1), T.scan_11(1, 2)] }
t(:scan_11_err) { T.scan_11 }
t(:scan_11_err2) { T.scan_11(1, 2, 3) }
t(:scan_rest) { [T.scan_rest(1, 2), T.scan_rest(1, 2, 3, 4)] }
t(:scan_rest_err) { T.scan_rest(1) }
t(:scan_02) { [T.scan_02, T.scan_02(1), T.scan_02(1, 2)] }
t(:scan_kw) { [T.scan_kw(1), T.scan_kw(1, k: 2), T.scan_kw({ h: 1 })] }
t(:scan_blk) { [T.scan_blk(2) { |x| x * 3 }, T.scan_blk(2)] }
t(:get_kwargs) { [T.get_kwargs(req: 1), T.get_kwargs(req: 1, opt2: 3)] }
t(:get_kwargs_missing) { T.get_kwargs(opt1: 1) }
t(:get_kwargs_unknown) { T.get_kwargs(req: 1, bad: 2) }

# misc
t(:ids) { T.ids }
t(:eval) { T.eval("1 + 2 * 3") }
t(:gvar) { [T.gvar, $cext_api_gv] }
t(:inspect_of) { [T.inspect_of("a\nb"), T.inspect_of(:sym), T.inspect_of([1, nil])] }
t(:freeze) { s = +"thaw"; [T.freeze(s), s.frozen?] }
t(:require) { [T.require("set"), T.require("set")] }
puts "done"
