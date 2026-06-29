def t; yield; rescue FrozenError; "FE"; end
p t { "abc  ".freeze.rstrip! }
p t { "abc".freeze.rstrip! }
p t { "  abc".freeze.lstrip! }
p t { " ab ".freeze.strip! }
s = "abc  ".dup
s.rstrip!
p s
s2 = "  x".dup
s2.lstrip!
p s2
