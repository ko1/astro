class C; include Enumerable; def each; [3,1,4,1,5,9,2,6].each { |v| yield v }; end; end
c = C.new
p c.min
p c.max
p c.min(3)
p c.max(3)
p c.min { |a, b| b <=> a }
p c.min_by { |x| -x }
p c.min_by(2) { |x| x }
p c.max_by(2) { |x| x }
class E; include Enumerable; def each; end; end
p E.new.min
p E.new.min(2)
