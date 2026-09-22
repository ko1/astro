# The C to_s printer has no form for Regexp / MatchData, so every path that
# renders a value has to ask the object: a composed pattern is built out of
# `Regexp#to_s`, and getting it wrong silently corrupts the pattern.
re = /a(b)c/
m = "xabc".match(re)

p [re].join
p [re, 1, :s, nil].join(",")
p ["x", re].join("-")
p [[re, [re]]].flatten.join("|")
p format("%s|%s", re, m)
p format("%-20s|", re)
p "#{re} #{m}"
p [m].join
p [re, m].map(&:to_s)
p({k: re}.to_s)
p [re].to_s
p "x".sub(/x/) { re }
p [re].inspect
