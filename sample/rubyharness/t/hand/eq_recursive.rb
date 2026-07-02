# == / eql? on recursive Array/Hash terminate (CRuby treats re-entry as equal),
# and nested hash/array values dispatch ==. vs ruby.
a = [1, 2]; 5.times { a << a }
b = [1, 2]; 5.times { b << b }
p(a == b)
p(a == a)
p(a.eql?(b))
c = [1, 2]; 5.times { c << c }; c << 99
p(a == c)
h1 = {x: 1}; h1[:s] = h1
h2 = {x: 1}; h2[:s] = h2
p(h1 == h2)
p({a: [1, 2]} == {a: [1, 2]})
p({a: [1, 2]} == {a: [1, 3]})
