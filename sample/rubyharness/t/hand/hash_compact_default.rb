h = Hash.new(99)
h[:a] = 1; h[:b] = nil; h[:c] = 3
c = h.compact
p c
p c[:missing]
hp = Hash.new { |hash, k| "dp-#{k}" }
hp[:x] = 1; hp[:y] = nil
cp = hp.compact
p cp
p cp[:z]
