# Hash#dig/Array#dig dispatch #dig on a user object, and Hash#dig applies the
# receiving Hash's default (value or proc) on a miss. vs ruby.
h = {}
h[:foo] = [ { bar: [ 1 ] }, [ obj = Object.new, 'str' ] ]
def obj.dig(*args); [ 42, args ]; end
p h.dig(:foo, 0, :bar)
p h.dig(:foo, 0, :bar, 0)
p h.dig(:foo, 1, 0, :anything)
p [obj, 1].dig(0, :a, :b)
begin; h.dig(:foo, 0, :bar, 0, :too_deep); rescue => e; p e.class; end
default = { bar: 42 }
hd = Hash.new(default)
p hd.dig(:foo).equal?(default)
p hd.dig(:foo, :bar)
p Hash.new(99).dig(:x)
p (Hash.new { |hh, k| k.to_s * 2 }).dig(:y)
p ({ a: { b: 1 } }).dig(:a, :b)
p ({ a: { b: 1 } }).dig(:nope)
