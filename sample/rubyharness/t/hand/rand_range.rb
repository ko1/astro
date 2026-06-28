srand(42)
p rand(1..6).class
p rand(1...6).class
p rand(1.5..2.5).class
p rand(-4).class
p rand(0).class
p (1..6).include?(rand(1..6))
p (1...6).include?(rand(1...6))
class TI; def to_int; 5; end; end
p rand(TI.new).class
p [rand(1..1), rand(5..5)]
