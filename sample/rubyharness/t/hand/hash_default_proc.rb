h = Hash.new { |hash, k| "dp:#{k}" }
p h.default(5)
p h.default
h2 = Hash.new(7)
p h2.default(1)
p h2.default
p h.default(nil)
