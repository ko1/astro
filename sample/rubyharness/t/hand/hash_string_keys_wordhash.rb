# String keys of every length hash consistently (the word-at-a-time string hash): lookups, eql? pairs, big tables
keys = (0..40).map { |n| "k" * n } + %w[a ab abc abcdefgh abcdefghi é 日本語 ＡＢ]
h = {}
keys.each_with_index { |k, i| h[k] = i }
p keys.all? { |k| h[k] == keys.index(k) }, h.size
p h["abcdefgh"], h["abcdefgh".dup], h["abcdefgh".b], h[:abcdefgh]
p "abc".hash == "abc".dup.hash, "abc".hash == "abd".hash, "".hash == "".dup.hash
big = {}
5000.times { |i| big["key-#{i}-#{'x' * (i % 23)}"] = i }
p big.size, big["key-4999-#{'x' * (4999 % 23)}"], big["key-0-"], big["nope"]
s = "mutable"; m = { s => 1 }; s << "!"; p m[s], m["mutable"], m.size
p [["a", "b"], ["a", "b"]].uniq.size, %w[x y x z y].tally, %w[aa bb aa].group_by(&:itself).keys
