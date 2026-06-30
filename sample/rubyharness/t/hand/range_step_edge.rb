# Range#step edge cases: step(0), beginless, String-range integer step. vs ruby.
begin; (1..10).step(0); rescue => e; p e.class; end
begin; (1.0..10.0).step(0.0); rescue => e; p e.class; end
begin; (..5).step(1) { |x| }; rescue => e; p e.class; end
begin; (1..10).step(0) { |x| }; rescue => e; p e.class; end
# String range stepped by an Integer (no block → ArithmeticSequence)
p ("a".."e").step(2).to_a
p ("a".."j").step(3).to_a
p ("aa".."ae").step(2).to_a
p ("a".."e").step.to_a
