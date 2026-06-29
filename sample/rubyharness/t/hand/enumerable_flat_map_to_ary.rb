class C; include Enumerable; def each; yield 1; yield 2; end; end
c = C.new
p c.flat_map { |x| [x, x * 10] }
p c.flat_map { |x| x }
class TA; def to_ary; [:a, :b]; end; end
p c.flat_map { |x| x == 1 ? TA.new : x }
class TB; def to_ary; 5; end; end
def t; yield; rescue TypeError; "TE"; end
p t { c.flat_map { |x| TB.new } }
p c.collect_concat { |x| [x, -x] }
