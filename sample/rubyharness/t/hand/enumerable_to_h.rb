class C; include Enumerable; def each; yield [:a, 1]; yield [:b, 2]; end; end
p C.new.to_h
p C.new.to_h { |k, v| [k.to_s, v * 10] }
class TA; def to_ary; [:x, 9]; end; end
class D; include Enumerable; def each; yield TA.new; end; end
p D.new.to_h
def t; yield; rescue => e; e.class; end
class E; include Enumerable; def each; yield [1, 2, 3]; end; end
p t { E.new.to_h }
class F; include Enumerable; def each(arg); yield [arg, 1]; end; end
p F.new.to_h(:z)
