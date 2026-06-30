# Integer#upto/downto with a non-numeric endpoint raise ArgumentError (comparison
# failed), not TypeError. vs ruby.
begin; 5.upto("x") { |i| }; rescue => e; p e.class; end
begin; 5.downto([1]) { |i| }; rescue => e; p e.class; end
begin; 5.upto(Object.new) { |i| }; rescue => e; p e.class; end
r = []; 1.upto(5) { |i| r << i }; p r
r = []; 5.downto(1) { |i| r << i }; p r
r = []; 1.upto(3.7) { |i| r << i }; p r
p 1.upto(5).to_a
p 5.downto(1).to_a
