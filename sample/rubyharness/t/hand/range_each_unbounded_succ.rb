# A range with no upper bound must NOT be materialized by a block-less #each:
# `(1..).each` and `(1..Float::INFINITY).each` hand back a #succ-driven
# enumerator, the way koruby already did for `("a"..)`.  #size is numeric-only.
#
# And String#succ carries only within one alphanumeric RUN: `"Collision_9".succ`
# is `"Collision_10"`, not `"Collisioo_0"` (rspec-core names colliding example
# groups that way).
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 60]]
end

t(:inf_each_cls)  { (1..Float::INFINITY).each.class }
t(:inf_each_3)    { (1..Float::INFINITY).each.first(3) }
t(:inf_each_size) { (1..Float::INFINITY).each.size }
t(:endless_3)     { (1..).each.first(3) }
t(:endless_size)  { (1..).each.size }
# 除外: `Enumerator.new { 無限 }.with_index` (非 lazy) は materialize してしまう
# 既存バグで、`(1..).each` が enumerator を返すようになって経路が増えただけ。
t(:str_endless)   { ("a"..).each.first(3) }
t(:str_end_size)  { ("a"..).each.size }
t(:fin_each)      { (1..5).each.to_a }
t(:fin_size)      { (1..5).each.size }
t(:str_fin)       { ("a".."c").each.to_a }
t(:str_fin_size)  { ("a".."c").each.size }
t(:inf_with_blk)  { r = []; (1..Float::INFINITY).each { |x| break if x > 3; r << x }; r }
t(:endless_blk)   { r = []; (1..).each { |x| break if x > 3; r << x }; r }
t(:inf_lazy)      { (1..Float::INFINITY).lazy.map { |x| x * 2 }.first(3) }
t(:inf_step)      { (1..Float::INFINITY).step(2).first(3) }

SUCC = ["Collision_9", "a9", "Az", "zz", "a_9", "1.9", "1.8.7", "-9", "9", "z", "Zz", "abcd",
        "THX1138", "<<koala>>", "***", "1999zzz", "ZZZ9999", "***a", "a-9", "A_z", "ab_zz",
        "", " ", "a b", "x_9_9", "1_9", "9_9", "Az_9", "a__9", "Collision_99"]
SUCC.each { |s| p [s, (s.succ rescue $!.class)] }
p ["Collision_9".next, ("a9".."b1").to_a, ("Az".."Bc").to_a.size]
