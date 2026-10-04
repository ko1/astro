# Array#join of plain Strings (sized and copied once) keeps CRuby's results: encodings, separators, subclasses, errors
p %w[a b c].join, %w[a b c].join("-"), ["x"].join(","), ["", ""].join("|"), %w[a b].join("")
p ["é", "a"].join("-").encoding, ["a", "é"].join.encoding, ["a".b, "b"].join.encoding, ["a".force_encoding("US-ASCII"), "b"].join.encoding
p ["a", "b"].join("é").encoding, ["\xff".b, "é".b].join.bytes
begin
  ["\xff".b, "é"].join
rescue => e
  p e.class
end
class S < String; end
p [S.new("q"), "r"].join("+"), [S.new("q")].join.class
p ["a", 1, :b, nil, "c"].join("/"), [["x", "y"], "z"].join("-")
big = Array.new(500) { |i| "s#{i}" }
j = big.join(", ")
p j.size, j[0, 20], j.end_with?("s499")
r = %w[a b].join("-"); r << "!"; p r, r.frozen?
3.times { |i| GC.start if i == 1; p Array.new(50) { "ab" * 10 }.join(".").size }
