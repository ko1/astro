h = { a: { b: { c: 1 } } }
def t; yield; rescue ArgumentError; "AE"; rescue TypeError; "TE"; end
p t { h.dig }
p h.dig(:a, :b, :c)
p h.dig(:a, :x)
p t { [1, 2].dig }
p [[1, [2, 3]]].dig(0, 1, 0)
