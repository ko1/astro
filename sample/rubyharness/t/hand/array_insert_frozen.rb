def t; yield; rescue FrozenError; "FE"; end
p t { [1, 2].freeze.insert(0, 9) }
p t { [1, 2].freeze.insert(0) }
p [1, 2].dup.insert(1, :x)
p [1, 2].dup.insert(5, :pad)
