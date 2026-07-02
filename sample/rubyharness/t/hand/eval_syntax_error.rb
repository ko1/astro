# eval with a syntax error raises SyntaxError (< ScriptError), not ArgumentError,
# and doesn't kill the process. vs ruby.
begin
  eval("1 +")
rescue SyntaxError => e
  p e.class
end
p SyntaxError.ancestors.include?(ScriptError)
p SyntaxError.ancestors.include?(StandardError)   # false — not rescued by bare rescue
p(eval("3 * 4"))
begin
  eval("def m(", binding)
rescue SyntaxError
  p :binding_ok
end
# a bare `rescue` must NOT catch SyntaxError (it's not a StandardError)
result = begin
  eval("<<<")
rescue => e
  :caught_standard
rescue SyntaxError
  :caught_syntax
end
p result
