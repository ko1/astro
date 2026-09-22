# A generator-backed lazy (`Enumerator.new { }.lazy`) must STREAM through
# with_index like an Array- or Range-backed one.  Chaining used to materialise
# the source instead, which runs away on an infinite generator (it corrupted the
# heap before the GC noticed).  The block form runs for its side effect only:
# the element that flows on is the ORIGINAL one, not the block's result.
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 60]]
end

inf = Enumerator.new { |y| i = 0; loop { y << i; i += 1 } }
fin = Enumerator.new { |y| [3, 1, 2].each { |x| y << x } }

t(:gen_wi)      { inf.lazy.with_index.first(3) }
t(:gen_wi_off)  { inf.lazy.with_index(10).first(3) }
t(:gen_wi_blk)  { inf.lazy.with_index { |x, i| [x, i] }.first(3) }
t(:gen_map_wi)  { inf.lazy.map { |x| x * 2 }.with_index.first(3) }
t(:gen_ewi)     { inf.lazy.each_with_index.first(3) }
t(:gen_wi_sel)  { inf.lazy.with_index.select { |x, i| i.even? }.first(2) }
t(:gen_chain)   { inf.lazy.with_index.map { |x, i| x + i }.first(4) }
t(:fin_wi)      { fin.lazy.with_index.to_a }
t(:fin_wi_blk)  { fin.lazy.with_index { |x, i| x + i }.to_a }
t(:gen_side)    { acc = []; r = fin.lazy.with_index(2) { |x, i| acc << [x, i] }.first(2); [r, acc] }

# the Array / Range sources keep behaving, block form included
t(:ary_wi)      { [3, 1, 2].lazy.with_index.to_a }
t(:ary_wi_off)  { [3, 1, 2].lazy.with_index(5).to_a }
t(:ary_wi_blk)  { [3, 1, 2].lazy.with_index { |x, i| x + i }.to_a }
t(:ary_side)    { acc = []; r = [3, 1, 2].lazy.with_index { |x, i| acc << [x, i] }.to_a; [r, acc] }
t(:rng_wi)      { (1..Float::INFINITY).lazy.with_index.first(3) }
t(:rng_wi_blk)  { (1..3).lazy.with_index { |x, i| x * 10 }.to_a }
t(:nonlazy_map) { [3, 1, 2].each.with_index.map { |x, i| x + i } }
