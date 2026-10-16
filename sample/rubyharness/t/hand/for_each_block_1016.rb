# `for x in obj` (not an Array / Range) is obj.each { |x| body }: lazy (an infinite Enumerator with break), the
# body between the yields, next / break / return, |a, b| targets, @iv targets, Proc#source_range / syntax_tree (2026-10-16)
iter = Object.new
def iter.each(&block) = block.call(block)
for b in iter
  42
end
p b.class, File.basename(b.source_location[0]), b.source_location[1], b.source_range.to_s[/\(.*/], b.syntax_tree.class
e = Enumerator.new { |y| i = 0; loop { y << (i += 1) } }
for x in e; break if x > 3; end
p x
log = []
o = Object.new
def o.each; yield 1; @l << :between; yield 2; end
o.instance_variable_set(:@l, log)
for v in o; log << v; end
p log
h = {a: 1, b: 2}
for k, w in h; log << [k, w]; end
p log.last(2), k, w
r = for q in o; next if q == 1; break q * 10; end
p r
def m2; for z in Enumerator.new { |y| y << 5 }; return z + 1; end; :no; end
p m2
s = 0
for i in 1..3; s += i; end
for j in [4, 5]; s += j; end
p s, i, j
for @iv in o; end
p @iv
for a1, b1 in [[1, 2], [3, 4]]; end
p [a1, b1]
c = 0
for t in o; [1].each { c += t }; end
p c
