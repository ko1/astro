class C; include Enumerable; def each; (1..7).each { |x| yield x }; end; end
c = C.new
class TI; def to_int; 3; end; end
p c.each_slice(3).to_a
p c.each_slice(TI.new).to_a
p (begin; c.each_slice(0).to_a; rescue ArgumentError; "AE"; end)
p c.each_cons(2).to_a
