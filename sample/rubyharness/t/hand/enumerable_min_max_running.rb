class C; include Enumerable; def initialize(*a); @a = a; end; def each; @a.each { |x| yield x }; end; end
p C.new(2, 33, 4, 11).min { |a, b| a <=> b }
p C.new(2, 33, 4, 11).min { |a, b| b <=> a }
p C.new(1, 2, 3, 4).min { |a, b| 15 }
p C.new(11, 12, 22, 33).min { |a, b| 2 }
p C.new(4, 1, 3, 2).min
p C.new(4, 1, 3, 2).max
p C.new(4, 1, 3, 2).min(2)
p C.new(4, 1, 3, 2).max(2)
p C.new(1, 2, 3, 4).max { |a, b| 15 }
def t; yield; rescue ArgumentError; "AE"; end
p t { C.new(1, nil).min }
