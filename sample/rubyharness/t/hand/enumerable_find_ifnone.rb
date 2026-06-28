class C; include Enumerable; def each; yield 1; yield 2; yield 3; end; end
c = C.new
p c.find { |x| x > 5 }
p c.find(-> { "none" }) { |x| x > 5 }
p c.find(-> { "none" }) { |x| x == 2 }
