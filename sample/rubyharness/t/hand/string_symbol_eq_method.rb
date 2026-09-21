p ["a" == "a", "a" == "b", "a" == :a, "a" == 1, "a" == nil]
p [String.instance_method(:==).owner, Symbol.instance_method(:==).owner]
p ["a".send(:==, "a"), "a".public_send(:==, "b")]
p [:a == :a, :a == :b, :a == "a", :a.send(:==, :a)]
p ["a" === "a", "a" === "b", :a === :a]
class S2 < String; end
p [S2.new("a") == "a", "a" == S2.new("a"), S2.new("a").eql?("a")]
class ToStr; def to_str = "a"; def ==(o) = "CALLED"; end
p ["a" == ToStr.new, ToStr.new == "a"]
p ["a".eql?(ToStr.new)]
e1 = "a".dup.force_encoding("ASCII-8BIT")
p ["a" == e1, "a".eql?(e1)]
p ["\xff".b == "\xff".b]
p [%w[= - +].include?("-"), %w[= - +].index("+")]
p ["a" <=> 1, "a".casecmp("A"), "a".casecmp?("A")]
h = {"a" => 1}
p [h["a"], h.key?("a")]
p ["abc".start_with?("a"), "abc".sub("b", "x")]
p [[1, "a", :b].include?("a"), ["a"].uniq, %w[a a b].tally]

# Comparable#== must not be what answers for String/Symbol: it runs the OTHER
# object's #<=>, so an unrelated object could raise from inside include?.
class Ch
  include Comparable
  def <=>(other) = (raise NoMethodError, "boom" unless other.is_a?(Ch); 0)
  def to_ary = [1]
end
ch = Ch.new
p (["-"].include?(ch) rescue [$!.class, $!.message[0, 30]])
p (["-"].index(ch) rescue [$!.class, $!.message[0, 30]])
p ([1].include?(ch) rescue [$!.class, $!.message[0, 30]])
p ([:x].include?(ch) rescue [$!.class, $!.message[0, 30]])
p (["-"].delete(ch) rescue [$!.class, $!.message[0, 30]])
p ({"-" => 1}.key?(ch) rescue [$!.class, $!.message[0, 30]])
