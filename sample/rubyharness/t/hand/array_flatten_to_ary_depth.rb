# Array#flatten coerces elements via #to_ary (nil to_ary -> leaf), and coerces
# the depth arg via #to_int. vs ruby.
class TA; def to_ary; [4, 5]; end; end
class NilAry; def to_ary; nil; end; end
class TI; def to_int; 1; end; end
p [1, 2, 3, TA.new].flatten
p [1, [2, TA.new]].flatten
p [1, NilAry.new, 2].flatten.size           # NilAry stays a leaf
p [1, NilAry.new, 2].flatten.last.is_a?(NilAry)
p [1, [2, [3]]].flatten(TI.new)
begin; [1, [2]].flatten("x"); rescue => e; p e.class; end
class BadAry; def to_ary; 5; end; end
begin; [1, BadAry.new].flatten; rescue => e; p e.class; end
p [1, [2, [3]]].flatten(2)
