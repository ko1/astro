class X; def to_int; $inner = caller(0); 0; end; end
def outer; caller(X.new, 1); end
puts "a:", outer, "inner0:", $inner[0]
class Ary; def to_ary; [1, 2]; end; end
def foo; yield Ary.new; end
def a; foo { |x, y| puts "b:", caller(0) }; end
a
def y0; yield; end
def g; caller(0); end
def h; y0; rescue LocalJumpError; send(:g); end
puts "c:", h
