class C; include Enumerable; def each; (1..6).each { |x| yield x }; end; end
c = C.new
p c.each_slice(2).is_a?(Enumerator)
p c.each_slice(2).to_a
p c.each_cons(2).is_a?(Enumerator)
p c.each_cons(2).to_a
