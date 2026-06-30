p "aaa".gsub("a") { |m| m.upcase }
p "hello world".gsub("o") { |m| "[#{m}]" }
p "hello".sub("l") { |m| m * 2 }
p "a1b2c3".gsub("1") { "X" }
p "hello".gsub("l", "L")
p "foofoo".gsub("foo") { |m| m.length.to_s }
s = "hello".dup
s.gsub!("l") { |m| m.upcase }
p s
p "test".gsub("t") { |m| m.ord.to_s }
count = 0
r = "aaa".gsub("a") { |m| count += 1; count.to_s }
p [r, count]
p "hello world".gsub(" ") { "_" }
p "abc".gsub("x") { "Y" }
p "hello".gsub("l") { |m| m == "l" ? "L" : m }
