# "the block passed to 'm' defined at ... may be ignored": which methods use their block (yield, defined?(yield),
# &b, &, ..., super), once per method/class/call shape, Method#call, Warning[:strict_unused_block] (2026-10-16)
$log = []
module Warning
  def self.warn(msg, **) = $log << msg.gsub(/\S*\//, "")
end
def flush(tag) = (puts "#{tag}: #{$log.inspect}"; $log.clear)
$VERBOSE = true
def a; 42; end
def b; block_given?; end
def c; binding; end
def g(*); end
def i(...) = 1
def j; lambda {}; end
def l; [1].each { }; end
def mm; defined?(yield); end
def w; [1].each { yield }; end
def x2; -> { yield }; end
def y(&) = 1
def y2(&b) = 1
def kw(a:) = 1
class Foo; def m; end; def self.s; end; def initialize; end; end
class Par; def m; end; end
class Chi < Par; def m; super; end; end
class Chi2 < Par; def m; super(); end; end
o = Object.new
def o.sing; end
module M; def q; end; module_function :q; end
a {}; a {}
2.times { a {} }
flush(:once_per_shape)
b {}; c {}; g {}; i {}; j {}; l {}; mm {}; w {}; x2 {}; y {}; y2 {}; kw(a: 1) {}
flush(:uses)
Foo.new.m {}; Foo.s {}; Foo.new {}; o.sing {}; M.q {}; Chi.new.m {}; Chi2.new.m {}
flush(:names)
pr = proc {}
def p1; end
p1(&pr); p1(&pr); p1 {}; p1(&nil)
flush(:blockarg)
def mc; end
me = method(:mc); me.call {}; me.call {}; mc {}
flush(:method_call)
def ini; end
alias ali ini
ali {}; ini {}
flush(:alias)
$VERBOSE = false
def quiet; end
quiet {}
flush(:not_verbose)
Warning[:strict_unused_block] = true
def strict; end
strict {}
Warning[:strict_unused_block] = false
flush(:strict)
