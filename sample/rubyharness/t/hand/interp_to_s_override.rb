# String interpolation must use a user-defined (or refined) #to_s, not the
# built-in printer.
class Hash; def to_s = "HASH_TO_S"; end
class Array; def to_s = "ARY_TO_S"; end
class Integer; def to_s(*a) = "INT_TO_S"; end
p ["#{{a: 1}}", "#{[1]}", "#{5}"]
p [{a: 1}.to_s, [1].to_s, 5.to_s]
p "#{nil}#{true}"
p "#{:sym}"
p "#{1.5}"
