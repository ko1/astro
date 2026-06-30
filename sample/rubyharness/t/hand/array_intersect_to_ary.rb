# Array#intersect? coerces a non-Array arg via #to_ary. vs ruby.
class TA; def to_ary; [2, 3]; end; end
p [1, 2, 3].intersect?(TA.new)
p [1, 2, 3].intersect?([4, 5])
p [1, 2, 3].intersect?([3, 4])
p [4, 5].intersect?(TA.new)
begin; [1].intersect?(5); rescue => e; p e.class; end
begin; [1].intersect?(Object.new); rescue => e; p e.class; end
