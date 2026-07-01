# String#[] / #slice (and Symbol#[]) accept Float / #to_int range bounds,
# converting them to Integer. vs ruby.
p "hello"[1.5..3.5]
p "hello"[1.5...3.5]
p "hello"[..2.9]
p "hello"[1.5..]
p "hello".slice(0.9, 2.9)
p :hello[1.5..3.5]
p "hello"[1..3]
p "hello"[-3..-1]
