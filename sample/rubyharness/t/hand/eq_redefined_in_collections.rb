# Reopening #== on String / Integer reaches the element comparisons of Array#index /
# include? / count / rindex / delete / == and Hash#value? / key / == (CRuby's rb_equal).
p ["a"].index("z"), {a: "a"}.value?("z"), ["a"].include?("z"), ["a"].count("z"), ({"k" => 1}.key(1)), (["a"] - ["z"]), ["a"].delete("z")
class String
  def ==(other) = true
end
p ["a"].index("z"), {a: "a"}.value?("z"), ["a"].include?("z"), ["a"].count("z"), ["a", "b"].rindex("z"), {a: "a"}.key("z"), ["a"].delete("z"), ({x: "a"} == {x: "b"}), (["a"] == ["b"])
class Integer
  def ==(o) = o == :one || equal?(o)
end
p [1, 2].index(:one), [5].include?(:one), {a: 1}.value?(:one)
