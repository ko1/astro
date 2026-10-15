# #hash call counts (rehash / colliding keys), shared range literal, temp name of nested modules,
# const_added caller line in module_eval, Lazy#force(*args) (2026-10-16)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
$log = []
K1 = Object.new; def K1.hash = ($log << :k1; 0)
K2 = Object.new; def K2.hash = ($log << :k2; 0)
t(:build) { $log.clear; h = { K1 => 1, K2 => 2 }; [$log, h.size] }
t(:rehash) { h = { K1 => 1, K2 => 2 }; $log.clear; h.rehash; $log }
t(:lookup) { h = { K1 => 1, K2 => 2 }; $log.clear; [h[K2], $log] }
t(:rehash_dedup) { a = [1, 2]; b = [1]; h = { a => 1, b => 2 }; b << 2; h.rehash; h }
t(:range_lit) { ary = []; 2.times { ary.push(1..3) }; [ary[0].equal?(ary[1]), ary[0].frozen?] }
t(:range_lit_open) { ary = []; 2.times { ary.push((1..), (..2), (nil..nil), (1...4)) }; ary.each_slice(4).to_a.transpose.map { _1.equal?(_2) } }
t(:range_lit_dyn) { ary = []; x = 1; 2.times { ary.push(x..3) }; ary[0].equal?(ary[1]) }
t(:temp_name_nested) do
  m = Module.new; m::N = Module.new
  a = m::N.name.sub(/0x\h+/, "X"); m.set_temporary_name "m"; b = m::N.name; m.set_temporary_name nil
  [a, b, m::N.name, m.name]
end
ConstAddLines = []
CAM = Module.new { def self.const_added(n) = ConstAddLines << [n, caller_locations(1, 1)[0].lineno - __LINE__] }
CAM.module_eval(<<-RUBY, __FILE__, __LINE__ + 1)
  TEST = 1

  module SubModule
  end
  self::Z = 2
RUBY
CAM.const_set(:CONST_SET, 1)
t(:const_added_lines) { ConstAddLines }
YM = Object.new
def YM.each(*a) = (yield a; yield 1, a)
t(:enum_to_a_args) { YM.to_enum.to_a(:x) }
t(:lazy_force_args) { YM.to_enum.lazy.force(:x) }
t(:lazy_to_a_args) { YM.to_enum.lazy.to_a(:x) }
t(:lazy_chain_args) { YM.to_enum.lazy.map { _1 }.force(:y) }
t(:lazy_plain) { (0..Float::INFINITY).lazy.map(&:succ).take(2).force }
