# error_highlight on koruby: RubyVM::AbstractSyntaxTree.node_id_for_backtrace_location finds the node again
# with Prism (by the exception's name, a call's block, the callee's def); snippets must match CRuby's
def t
  yield
rescue => e
  puts e.detailed_message(highlight: false)
end
x = nil
t { x.foo(1) }
t { [1].map { |v| v.bar } }
t { 1 + nil }
t { NoSuchConst::X }
t { Integer::NoSuch }
t { undefined_local_or_meth }
h = {a: 1}
t { h.fetch(:b) }
t { x.foo; x.bar }
p ErrorHighlight.spot(RuntimeError.new("x")) rescue p $!
def a(x) = x
t { a(1.foo(nil)) }
t { [1, 2].each { |i| i.nosuch } }
t { Integer.sqrt("x") }
