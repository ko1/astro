# Enumerable#chunk_while / #slice_when require a block (ArgumentError otherwise). vs ruby.
class E; include Enumerable; def each; yield 1; yield 2; yield 4; yield 5; end; end
e = E.new
p e.chunk_while { |i, j| i + 1 == j }.to_a
p e.slice_when { |i, j| i + 1 != j }.to_a
begin; e.chunk_while; rescue => err; p err.class; end
begin; e.slice_when; rescue => err; p err.class; end
