# Array#min_by/max_by/minmax_by return an Enumerator with no block; Array#take
# coerces n via #to_int; Enumerable#minmax_by no-block returns an Enumerator. vs ruby.
p [1, 2, 3].min_by.class
p [1, 2, 3].max_by.class
p [1, 2, 3].minmax_by.class
p [1, 2, 3].min_by { |x| -x }
p [1, 2, 3].max_by { |x| -x }
p [3, 1, 2].minmax_by { |x| x }
class TI; def to_int; 2; end; end
p [1, 2, 3, 4].take(TI.new)
begin; [1, 2, 3].take("x"); rescue => e; p e.class; end
class E; include Enumerable; def each; yield 3; yield 1; yield 2; end; end
p E.new.minmax_by { |x| x }
p E.new.minmax_by.class
