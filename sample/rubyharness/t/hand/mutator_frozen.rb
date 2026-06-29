def t; yield; rescue FrozenError; "FE"; rescue => e; e.class.to_s; end
p t { "ABC".freeze.upcase! }
p t { "abc".freeze.upcase! }
p t { "".freeze.reverse! }
p t { "abc".freeze.reverse! }
p t { "aabb".freeze.squeeze! }
p t { "abcd".freeze.squeeze! }
p t { "abc".freeze.chomp! }
p t { [1, 2].freeze.reverse! }
p t { "abc".freeze.capitalize! }
p t { "Abc".freeze.capitalize! }
p t { "AbC".freeze.swapcase! }
p t { "ABC".freeze.downcase! }
p t { [3, 1, 2].freeze.rotate! }
p "abc".dup.upcase!
p "ABC".dup.downcase!
p [1, 2, 3].dup.reverse!
