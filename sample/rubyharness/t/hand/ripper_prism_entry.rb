# Ripper goes through the bundled prism (C dump + that same version's Ruby
# Serialize).  `Prism::Translation::Ripper` asks for version: "current", and a
# mismatched prism gem must not win the `require "prism"` race -- either one
# makes Ripper.sexp die with "Invalid serialization".
require "ripper"
require "prism"
p Ripper.sexp("1 + 2")
p Ripper.sexp("def m(a, b = 1, *r, k:, &blk) = a")
p Ripper.lex("x = 1").reject { |_, t, _, _| t == :on_sp }
p Ripper.sexp("1 +")            # a syntax error is nil, not a raise
p Prism.parse("1 + 2").value.class
p Prism.parse("a = 1", version: "current").success?
p Prism.parse("a = 1", version: "latest").success?
p Prism.parse("a = 1", version: "3.4.0").success?
begin
  Prism.parse("a = 1", version: "2.9")
rescue ArgumentError => e
  p e.message
end
