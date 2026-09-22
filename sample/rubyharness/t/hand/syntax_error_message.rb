# SyntaxError#message carries prism's formatted block, like CRuby.  Only
# single-diagnostic cases are compared: koruby's bundled prism emits several
# diagnostics for one position in a different order than CRuby's copy.
def try_eval(src)
  eval(src, nil, "(t)")
rescue SyntaxError => e
  [e.class, e.message]
end
p try_eval("def m(; end")
p try_eval("@1 = 2")
p (eval("1+1"))
p (eval("x = 5; x * 2"))
