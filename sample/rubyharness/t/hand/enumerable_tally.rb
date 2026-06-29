p ["a", "b", "a", "c", "a"].tally
h = Hash.new(100)
p ["foo", "foo", "bar", "baz"].tally(h)   # default value must be ignored (start at 0)
p ["z", "w"].tally({ "z" => 1 })          # pre-populated literal hash increments existing key
p ["a", "a"].tally({ "a" => 5 })
p [1, 1, 2, 3, 3, 3].tally
