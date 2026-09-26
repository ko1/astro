# Regexp: a greedy group repeated a counted number of times matches (the backtrack memo
# must not cut it), and \W \D \S \H take a whole non-ASCII character.
p [/(?:a+){3}/ =~ "aaa", /(?:a+){2}/ =~ "aa", /(?:ab+){3}/ =~ "ababab", /(a+){3}/ =~ "aaa", /\w+{3,}/ =~ "abc", /(?:a+){3,}/ =~ "aaaa", /(?:a*){3}b/ =~ "aab"]
p [/\Wx/ =~ "°x", /\W/ =~ "°", "°x".match(/\Wx/)&.[](0), /[^\w]x/ =~ "°x", /\Dx/ =~ "°x", /\Sx/ =~ "°x", /\Hx/ =~ "°x"]
p [/a\W/.match("a°")[0], "°°".match(/\D{2}/)[0], /[\W]x/ =~ "°x", /[^\W]/ =~ "°", "é°".scan(/\S/)]
