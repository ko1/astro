# Array#join converts each element via #to_str, then #to_ary (recurse), then
# #to_s — in that order; the separator goes between elements only. vs ruby.
class ToStr; def to_str; "foo"; end; end
class ToAry; def to_ary; ["b", "c"]; end; end
class ToS; def to_s; "S"; end; end
p ["a", ToStr.new, "d"].join("-")
p ["a", ToAry.new, "d"].join("-")
p [1, [2, [3, 4]], 5].join("-")
p ["a", ToS.new].join("-")
p [1, 2, 3].join(", ")
p [].join("-")
