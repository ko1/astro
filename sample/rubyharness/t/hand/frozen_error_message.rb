def t; yield; rescue FrozenError => e; e.message; end
p t { "abc".freeze << "x" }
p t { [1, 2].freeze << 3 }
p t { { a: 1 }.freeze.store(:b, 2) }
p t { [1].freeze.delete(1) }
p t { "x".freeze.replace("y") }
