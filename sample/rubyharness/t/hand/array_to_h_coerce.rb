class Pair; def to_ary; [:k, :v]; end; end
p [[:a, 1], [:b, 2]].to_h
p [Pair.new].to_h
p (begin; [1, 2].to_h; rescue TypeError; "TE"; end)
p (begin; [[1,2,3]].to_h; rescue ArgumentError; "AE"; end)
p [[1,2],[3,4]].each.to_h rescue p "enum-err"
