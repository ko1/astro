def t; yield; rescue FrozenError; "FE"; end
# Array#delete: FrozenError only when an element actually matches (would modify)
p t { [1, 2, 3].freeze.delete(2) }
p([1, 2, 3].freeze.delete(0))
p([1, 2, 3].freeze.delete(9))
p([1, 2, 1].dup.delete(1))
p([1, 2].dup.delete(9) { :nf })
# String#chop!: FrozenError when non-empty (would modify); empty returns nil
p t { "abc".freeze.chop! }
p t { "".freeze.chop! }
p "abc".dup.chop!
p "ab\r\n".dup.chop!
