# Regexp#=~ / === operand conversion: nil = no match, a non-String without #to_str (or whose #to_str is not a String) is a TypeError
o = Object.new; def o.to_str = "abc"
q = Object.new; def q.to_str = 5
[nil, "xbx", :abc, o, /b/, 1, Object.new, q].each { |v| p(begin; /b/ =~ v; rescue => e; [e.class, e.message]; end) }
p(/b/ === /b/, /b/ === 1, /b/ === nil)
q = Object.new; def q.to_str = 5; p((/b/ === q rescue $!.message))
