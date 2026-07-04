# f(*obj): obj.to_a spreads; to_a returning non-Array raises TypeError; nil or no
# to_a wraps as [obj]. vs ruby.
class ToA; def to_a; [1, 2]; end; end
def m(*a); a; end
p m(*ToA.new)
def m2(a, b); [a, b]; end
p m2(*ToA.new)
class NilToA; def to_a; nil; end; end
p m(*NilToA.new).size
class NoToA; end
p m(*NoToA.new).size
class BadToA; def to_a; 5; end; end
begin; m(*BadToA.new); rescue TypeError => e; p :type; end
p [*ToA.new]
begin; [*BadToA.new]; rescue TypeError; p :arr_type; end
