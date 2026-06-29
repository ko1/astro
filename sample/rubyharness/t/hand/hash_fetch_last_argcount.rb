class TI; def to_int; 2; end; end
p [1, 2, 3, 4].last(TI.new)
p [1, 2, 3, 4].last(2)
p [1, 2, 3].last
def t; yield; rescue ArgumentError; "AE"; rescue TypeError; "TE"; end
p t { { a: 1 }.fetch }
p t { { a: 1 }.fetch(:a, 2, 3) }
p({ a: 1 }.fetch(:a))
p({ a: 1 }.fetch(:b, 99))
p t { [1].last(Object.new) }
