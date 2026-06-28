h = Hash.new(1)
h[:a] = 10
g = {}.replace(h)
p g[:missing]
p g[:a]
h2 = {}
h2.default = 99
src = {x: 1}
h2.replace(src)
p h2[:nope]
pr = Hash.new { |hash, k| "proc:#{k}" }
target = {}
target.replace(pr)
p target[:zzz]
