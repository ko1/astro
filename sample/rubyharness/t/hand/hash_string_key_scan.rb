# String keys in a small (linear-scan) Hash: same / different length, same first byte, empty key, an encoding
# difference on ASCII and on non-ASCII content, a String subclass key, mixed key types, deletes, and GC
class SubStr < String; end
3.times do |r|
  GC.start if r == 1
  h = { "a" => 1, "ab" => 2, "abc" => 3, "" => 0, :abc => :sym, 7 => :int, "b" => 4 }
  p h["a"], h["ab"], h["abc"], h[""], h[:abc], h[7], h["abd"], h["c"], h["b"], h["ab".dup], h[SubStr.new("abc")]
  p h["abc".b], h["abc".encode("US-ASCII")], h.key?("abc".force_encoding("ASCII-8BIT"))
  u = { "é" => :utf8 }; p u["é"], u["é".b], u["é".dup.force_encoding("ISO-8859-1")], u["é".b.force_encoding("UTF-8")]
  s = { SubStr.new("x") => :sub }; p s["x"], s[SubStr.new("x")]
  h.delete("ab"); p h["ab"], h["abc"], h.size
  big = {}; 20.times { |i| big["k#{i}"] = i }; p big["k3"], big["k19"], big["k20"]
end
