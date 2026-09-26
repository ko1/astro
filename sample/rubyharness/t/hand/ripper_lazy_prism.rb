# require "ripper" does not load prism (CRuby's ripper.so is independent of it:
# simplecov checks defined?(Prism)); prism is loaded on first use.
require 'ripper'
p defined?(Prism)
p Ripper.lex("a = 1 # c").map { _1[1] }
p Ripper.sexp("1+2")
class MySB < Ripper::SexpBuilder; end
p MySB.new("x").parse
p Ripper.tokenize("def f; end")
p Ripper.new("1").parse
