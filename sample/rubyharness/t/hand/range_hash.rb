# Range#hash is content-based (begin/end/exclude_end), so equal ranges hash
# equal and work as Hash keys / in uniq. vs ruby.
p (1..5).hash == (1..5).hash
p (1..5).hash == (1...5).hash
p (1..5).hash == (1..6).hash
p ("a".."z").hash == ("a".."z").hash
h = { (1..5) => "a", (1...5) => "b" }
p h[1..5]
p h[1...5]
p [(1..3), (1..3), (1..4), (1...3)].uniq.size
