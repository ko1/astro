# CRuby's rb_cmperr names the LEFT operand by its class and the RIGHT one by
# #inspect when it is an immediate or a Float, then says WHY the comparison
# failed: a numeric #coerce that was refused, or a <=> that returned nil.
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s.sub(/0x\h+/, "0xADDR")]
end

t(:int_lt_str)     { 1 < "a" }
t(:int_le_nil)     { 1 <= nil }
t(:int_gt_sym)     { 1 > :a }
t(:float_lt_nil)   { 1.0 < nil }
t(:rational_lt)    { Rational(1, 2) < "a" }
t(:str_lt_int)     { "a" < 1 }
t(:str_lt_float)   { "a" < 1.0 }
t(:sym_lt_int)     { :a < 1 }
t(:int_upto)       { 1.upto("a") { } }
t(:int_downto)     { 1.downto(:a) { } }
t(:int_step_str)   { 1.step("a") { } }
t(:int_step_nil_e) { 1.step(nil, 2).first(3) }

t(:ary_sort)       { [1, "a"].sort }
t(:ary_sort_blk)   { [1, "a"].sort { |x, y| x <=> y } }
t(:ary_max)        { [1, "a"].max }
t(:ary_min)        { [1, "a"].min }
t(:ary_sort_by)    { [1, "a"].sort_by { |x| x } }
# 除外: `minmax` と `max(n)` は CRuby と探索アルゴリズムが違うので、どちらの
# オペランドを blame するかの順序だけが揃わない (理由サフィックスは一致する)。
t(:enum_sort)      { [1, "a"].each_entry.sort }
t(:enum_min_by)    { [1, "a"].min_by { |x| x } }
t(:enum_group)     { [1, "a"].group_by { |x| x }.keys.sort }
t(:sort_nil_blk)   { [1, 2].sort { |a, b| nil } }

t(:int_clamp)      { 1.clamp("a", "b") }
t(:int_clamp_rng)  { 1.clamp("a".."b") }
t(:str_clamp)      { "a".clamp(1, 2) }
t(:nan_clamp)      { Float::NAN.clamp(1, 2) }
t(:float_clamp_s)  { 1.0.clamp("a", "b") }

CMP = Class.new { include Comparable; def <=>(o) = nil }
t(:comparable_lt)  { CMP.new < CMP.new }
t(:comparable_gt1) { CMP.new > 1 }
t(:comparable_bet) { CMP.new.between?(CMP.new, CMP.new) }
t(:comparable_cl)  { CMP.new.clamp(CMP.new, CMP.new) }

# the endless forms that must NOT raise (and must not spin)
t(:step_endless)   { r = []; 1.step { |x| break if x > 3; r << x }; r }
t(:step_endless_by) { r = []; 1.step(by: 2) { |x| break if x > 5; r << x }; r }
t(:step_bignum)    { r = []; 1.step(2**70) { |x| break if x > 3; r << x }; r }
t(:step_neg)       { 5.step(1, -2).to_a }
t(:step_float)     { 1.step(3.5).to_a }
