# A syntax error on a very long source line must raise SyntaxError, not abort.
# koruby built the diagnostic with `n += snprintf(...)`, which returns the length
# it WOULD have written; on a 16 KB line n ran past the 4 KB buffer and the next
# `sizeof buf - n` underflowed, so _FORTIFY_SOURCE killed the process
# (`*** buffer overflow detected ***`).  A 4000-deep nested call in an eval'd
# string is enough: prism reports a nesting-depth diagnostic on one huge line.
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class]
end

t(:short)  { eval("1 +") }
t(:long)   { eval("x" * 8000 + " = = 1") }
t(:nested) { eval("def deep_nested; " + (1..4000).inject("1") { |s, _| "id(#{s})" } + "; end") }
t(:after)  { 6 * 7 }

t(:msg_kind) do
  begin
    eval("y" * 8000 + " = = 1")
  rescue SyntaxError => e
    [e.message.include?("syntax error"), e.message.bytesize < 100_000]
  end
end
