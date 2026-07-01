# p / inspect of an Exception renders "#<Class: message>" (bare Class when the
# message is empty); puts / to_s renders the message. vs ruby.
p RuntimeError.new("x")
p RuntimeError.new
p ArgumentError.new("bad")
p StandardError.new("")
class MyErr < StandardError; end
p MyErr.new("oops")
p MyErr.new
p [RuntimeError.new("a"), ArgumentError.new("b")]
p({err: RuntimeError.new("z")})
begin; raise "boom"; rescue => e; p e; end
p StopIteration.new
puts RuntimeError.new("via puts")
p RuntimeError.new("x").to_s
p RuntimeError.new("x").inspect
