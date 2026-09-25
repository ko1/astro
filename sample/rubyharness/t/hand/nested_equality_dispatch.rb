# Equality inside a CONTAINER has to reach the element's own `==` / `eql?`.
# koruby only dispatched when the compared value itself was a user object, so
# anything one level down fell back to korb_value_eq, which knows String /
# Symbol / Hash / Range / Set but nothing else — a Struct, a Data, a Time, an
# Exception or a plain object then compared by identity.  rubocop groups AST
# nodes with `group_by { [enclosing, name, argument_node] }`, so its
# Gemspec/DuplicatedAssignment cop never saw the duplicates.
require "set"

def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 50]]
end

S = Struct.new(:a)
D = Data.define(:a)

class U
  attr_reader :v
  def initialize(v) = (@v = v)
  def hash = @v.hash
  def eql?(o) = o.is_a?(U) && o.v == @v
  alias == eql?
end

class OnlyEq                      # `==` but no hash/eql?
  attr_reader :v
  def initialize(v) = (@v = v)
  def ==(o) = o.is_a?(OnlyEq) && o.v == @v
end

t(:u_direct)     { U.new(1) == U.new(1) }
t(:u_in_array)   { [[U.new(1)]].include?([U.new(1)]) }
t(:u_index)      { [[U.new(1)]].index([U.new(1)]) }
t(:u_hash_key)   { { [U.new(1)] => :x }[[U.new(1)]] }
t(:u_key_p)      { { [U.new(1)] => :x }.key?([U.new(1)]) }
t(:u_uniq)       { [[U.new(1)], [U.new(1)]].uniq.size }
t(:u_group_by)   { [[U.new(1)], [U.new(1)]].group_by { |x| x }.size }
t(:u_tally)      { [[U.new(1)], [U.new(1)]].tally.values }
t(:u_set)        { Set[[U.new(1)], [U.new(1)]].size }
t(:u_minus)      { ([[U.new(1)], [U.new(2)]] - [[U.new(1)]]).size }
t(:u_and)        { ([[U.new(1)]] & [[U.new(1)]]).size }
t(:u_or)         { ([[U.new(1)]] | [[U.new(1)]]).size }
t(:u_delete)     { a = [[U.new(1)]]; a.delete([U.new(1)]); a }
t(:u_rindex)     { [[U.new(1)], [U.new(1)]].rindex([U.new(1)]) }
t(:u_count)      { [[U.new(1)], [U.new(1)]].count([U.new(1)]) }
t(:u_assoc)      { [[[U.new(1)], :v]].assoc([U.new(1)])&.last }

t(:struct_nested){ [[S.new(1)]].include?([S.new(1)]) }
t(:struct_key)   { { [S.new(1)] => 1 }.key?([S.new(1)]) }
t(:struct_uniq)  { [[S.new(1)], [S.new(1)]].uniq.size }
t(:data_nested)  { [[D.new(a: 1)]].include?([D.new(a: 1)]) }
t(:time_nested)  { [[Time.at(0)]].include?([Time.at(0)]) }
t(:exc_nested)   { [[RuntimeError.new("x")]].include?([RuntimeError.new("x")]) }
t(:only_eq)      { [[OnlyEq.new(1)]].include?([OnlyEq.new(1)]) }
t(:only_eq_uniq) { [[OnlyEq.new(1)], [OnlyEq.new(1)]].uniq.size }

t(:deep2)        { [[[U.new(1)]]].include?([[U.new(1)]]) }
t(:hash_in_arr)  { [[{ k: U.new(1) }]].include?([{ k: U.new(1) }]) }
t(:mixed)        { [[1, "s", :y, U.new(1)]].include?([1, "s", :y, U.new(1)]) }

# the plain paths must not change
t(:scalars)      { [[[1, 2]].include?([1, 2]), [["a"]].include?(["a"]), [[:a]].include?([:a])] }
t(:int_float)    { [[[1]].include?([1.0]), [[1]].eql?([[1.0]]), { [1] => :x }[[1.0]]] }
t(:nan)          { [[Float::NAN]].include?([Float::NAN]) }
t(:self_ref)     { a = [1]; a << a; b = [1]; b << b; [a == b, [a].include?(b)] }
t(:sort_nested)  { [[2, U.new(1)], [1, U.new(1)]].sort_by { |x| x[0] } .map { |x| x[0] } }
