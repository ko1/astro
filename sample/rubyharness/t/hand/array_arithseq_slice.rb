# Array#[] with an Enumerator::ArithmeticSequence (stepped-range slice). vs ruby.
a = [0,1,2,3,4,5]
p a[(0..).step(2)]
p a[(2..).step(-1)]
p a[(2..).step(-2)]
p a[(..3).step(1)]
p a[(...3).step(1)]
p a[(..-2).step(2)]
p a[(1..3).step(2)]
p a[(1...3).step(2)]
p a[(-4..4).step(2)]
p a[(0..).step(-1)]
b = [0,1,2,3,4,5,6,7,8,9]
p b[(1..5) % 2]
p b[(1...5) % 2]
p b[(5..1) % -2]
p b[(1..) % 3]
begin; b[(100..200) % 2]; rescue RangeError; p :re_begin; end
begin; b[(1..100) % 2]; rescue RangeError; p :re_end; end
