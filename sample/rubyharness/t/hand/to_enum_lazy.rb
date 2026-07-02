# to_enum returns a lazy Enumerator: the underlying method runs only when the
# enumerator is driven, and lazy chains never over-iterate. vs ruby.
class Src
  def each
    yield 1
    yield 2
    raise "should not reach: over-iterated"
  end
end

# building the enumerator must not run #each (no raise here)
e = Src.new.to_enum(:each)
p e.class == Enumerator

# lazy + first(2) stops before the raise
p Src.new.to_enum(:each).lazy.map { |x| x * 10 }.first(2)

# plain first(n) on the generator also bounds
p Src.new.to_enum(:each).first(2)

# ordinary finite enumerators still fully materialize
class Fin
  def each; yield 1; yield 2; yield 3; end
end
p Fin.new.to_enum(:each).to_a
p Fin.new.to_enum(:each).map { |x| x + 1 }
