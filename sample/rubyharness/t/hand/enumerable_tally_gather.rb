class Z; include Enumerable; def each; yield; yield 1; yield 1, 2; end; end
z = Z.new
p z.to_a
p z.tally
p z.map { |x| x }
class T; include Enumerable; def each; yield :a; yield :b; yield :a; end; end
p T.new.tally
p T.new.tally({:a => 10})
