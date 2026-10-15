# Kernel#eval reached by send / Method#call (receiver as self, caller's scope and definee),
# Invalid yield in a top-level eval, identifiers in the source's encoding, refinements of a
# binding (2026-10-16)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
class ES; method(:eval).call("def x1; end"); send(:eval, "def x3; end"); end
t(:definee) { [ES.method_defined?(:x1, false), ES.method_defined?(:x3, false), Object.private_method_defined?(:x1)] }
t(:kernel_self) { Kernel.eval("self") }
t(:recv_self) { o = Object.new; o.send(:eval, "self").equal?(o) }
t(:caller_locals) { a = 5; send(:eval, "a + 1") }
def mm; b = 7; method(:eval).call("b"); end
t(:method_call_locals) { mm }
t(:yield_top) { begin; eval("1.times { yield }"); rescue SyntaxError => e; e.class; end }
def ym; eval("[1].map { 2 }"); end
t(:no_yield_ok) { ym }
t(:enc_ident) {
  code = "# frozen_string_literal: true\n  # encoding: UTF-8\nclass ES\n  V\xCF\x80ff = 1\nend\n".b
  eval(code)
  ES.constants(false).include?("V\xCF\x80ff".b.to_sym)
}
t(:enc_ident_spaces) {
  code = "  # encoding: UTF-8\nclass ES\n  V\xCF\x80sp = 1\nend\n".b
  eval(code)
  ES.constants(false).include?(:"Vπsp")
}
class RefTarget; end
RefMod = Module.new { refine(RefTarget) { def rfoo = "bar" } }
b = nil
Module.new { using RefMod; b = binding }
t(:binding_refine) { eval("RefTarget.new.rfoo", b) }
t(:no_refine_outside) { RefTarget.new.rfoo }
