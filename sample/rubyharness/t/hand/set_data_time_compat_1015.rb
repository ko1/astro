# Set iteration guards / identity equality, Data#with / .new keywords, Kernel#===, fractional utc_offset (2026-10-15)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
t(:set_add_new) { s = Set[1, 2]; s.each { s.add?(3) } }
t(:set_add_old) { s = Set[1, 2]; s.each { s << 1 }; s }
t(:set_delete) { s = Set[1, 2]; s.each { s.delete(2) }; s }
t(:set_merge) { s = Set[1, 2]; s.each { s.merge([1]) } }
t(:set_replace) { s = Set[1, 2]; s.each { s.replace([3]) } }
t(:set_after) { s = Set[1]; s.each { }; s << 2 }
t(:set_break) { s = Set[1, 2]; s.each { break }; s << 3 }
t(:set_raise) { s = Set[1, 2]; (s.each { raise "x" } rescue nil); s << 3 }
t(:set_cbi) { [Set[1, 2].compare_by_identity == Set[1, 2], Set[1, 2] == Set[1, 2].compare_by_identity,
               Set[1].compare_by_identity == Set[1].compare_by_identity] }
t(:set_map) { s = Set[1].compare_by_identity; s.map! { _1 }; s.compare_by_identity? }
t(:set_flatten) { s = Set[1].compare_by_identity; [s.flatten!, s.compare_by_identity?] }
Measure = Data.define(:amount, :unit)
Over = Data.define(:amount, :unit) do
  def initialize(amount:, unit: "m") = super
end
Blk = Data.define(:amount) do
  def initialize(amount:, &block) = super(amount: block.call(amount))
end
t(:with_none) { d = Measure.new(amount: 1, unit: "km"); d.with.equal?(d) }
t(:with_some) { Measure.new(amount: 1, unit: "km").with(amount: 2) }
t(:with_frozen) { Measure.new(amount: 1, unit: "km").with(amount: 2).frozen? }
t(:with_pos) { Measure.new(amount: 1, unit: "km").with(1) }
t(:new_strkey) { k = Object.new; def k.to_str = "system"; Measure.new(amount: 1, unit: "km", k => 2) }
t(:over_strkey) { Over.new(amount: 1, "system" => 2) }
t(:over_default) { Over.new(amount: 1) }
t(:blk_new) { [Blk.new(1) { _1 * 2 }.amount, Blk.new(amount: 1) { _1 * 2 }.amount] }
t(:blk_bracket) { [Blk[1] { _1 * 2 }.amount, Blk[amount: 1] { _1 * 2 }.amount] }
Pt = Struct.new(:x) do
  def initialize(x, &b) = super(b ? b.call(x) : x)
end
t(:struct_bracket_blk) { Pt[3] { _1 + 1 }.x }
t(:kernel_eqq_identity) { o = Object.new; def o.==(x) = false; Kernel.instance_method(:===).bind_call(o, o) }
t(:kernel_eqq_truthy) { o = Object.new; def o.==(x) = 1; Kernel.instance_method(:===).bind_call(o, 2) }
t(:offset_float) { Time.new(2010, 4, 4, 1, 59, 59, 7245.5).utc_offset }
t(:offset_round) { [Time.new(2012, 1, 1, 0, 0, 0, Rational(36645, 10)).strftime("%::z"),
                    Time.new(2012, 1, 1, 0, 0, 0, -3664.5).strftime("%::z")] }
# Hash#map with a Method of arity 2, Enumerable#map hands #each a block of the caller's arity,
# Array#hash combines the elements' #hash (an element's non-Integer #hash goes through #to_int),
# IO#puts asks #method_missing for #to_ary
def pair_reg(a, b) = [a, b]
def one_reg(a) = a
t(:hash_map_method2) { { 1 => "a", 2 => "b" }.map(&method(:pair_reg)) }
t(:hash_map_method1) { { 1 => "a" }.map(&method(:one_reg)) }
ArityEach = Object.new
def ArityEach.each(&b) = ($arities << b.arity; yield 1; yield [2, 3])
ArityEach.extend(Enumerable)
$arities = []
t(:enum_map_arity) { [ArityEach.map { |a, b| [a, b] }, ArityEach.map { |a| a }, ArityEach.map { 1 }, $arities] }
t(:ary_hash_to_int) { h = Object.new; def h.to_int = 1.hash; o = Object.new; o.instance_variable_set(:@h, h); def o.hash = @h; [o].hash == [1].hash }
t(:ary_hash_nested) { [[1, [2]], "x"].hash == [[1, [2]], "x"].hash && [1, 2].hash != [2, 1].hash }
PutsMm = Object.new
def PutsMm.method_missing(n, *a) = ($mm << n; nil)
def PutsMm.to_s = "MM"
$mm = []
t(:puts_mm_to_ary) { $stdout.puts(PutsMm); $mm }
