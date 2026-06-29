class C; include Enumerable; def each; [1,2,4,2,1,2].each { |v| yield v }; end; end
c = C.new
p c.count
p c.count(2)
p c.count { |x| x.even? }
def t; yield; rescue ArgumentError; "AE"; end
p t { c.count(1, 2) }
