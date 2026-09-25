# Object#dup had no case for an Enumerator, so it fell through to "return the
# receiver" — `e.dup` WAS `e`.  External iteration state then leaked between
# what should be independent enumerators: driving one to exhaustion made every
# later `dup` start out already finished.
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 50]]
end

G = Enumerator.new { |y| 3.times { |i| y << i } }

def drain(e)
  r = []
  begin
    r << e.next while true
  rescue StopIteration
  end
  r
end

t(:not_same)    { !G.dup.equal?(G) }
t(:clone_same)  { !G.clone.equal?(G) }
t(:class_kept)  { G.dup.class }
t(:first_drive) { drain(G.dup) }
t(:second)      { drain(G.dup) }
t(:third)       { drain(G.dup) }
t(:original)    { drain(G) }
t(:after_orig)  { drain(G.dup) }
t(:to_a_twice)  { [G.to_a, G.to_a] }
t(:indep_cursor){ a = G.dup; b = G.dup; a.next; [a.next, b.next] }
t(:lazy_dup)    { l = (1..3).lazy.map { |x| x * 2 }; [!l.dup.equal?(l), l.dup.first(2)] }
t(:arr_enum)    { e = [1, 2].each; [!e.dup.equal?(e), drain(e.dup), drain(e.dup)] }
t(:md_dup)      { m = /(\w)/.match("ab"); [!m.dup.equal?(m), m.dup[1]] }
t(:ivar_indep)  { a = G.dup; a.instance_variable_set(:@x, 1); [G.instance_variable_defined?(:@x), a.instance_variable_get(:@x)] }
t(:size_kept)   { [G.size, G.dup.size, (1..3).each.size, (1..3).each.dup.size] }
