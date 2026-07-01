# Enumerable#partition and #drop_while gather multi-yield into a single Array
# element for the block (take_while/all?/count spread — 1-arg gets first). vs ruby.
class M
  include Enumerable
  def each; yield 1, 2; yield 3, 4, 5; yield 6; end
end
m = M.new
p m.partition { |e| e.is_a?(Array) }
p m.drop_while { |e| e.is_a?(Array) && e.size == 2 }
p m.take_while { |e| e.is_a?(Integer) }
p m.partition { |a, b| a == 3 }
# ordinary receivers unaffected
p [1, 2, 3, 4, 5].partition(&:even?)
p [1, 2, 3, 4, 5].drop_while { |x| x < 3 }
p({ a: 1, b: 2, c: 3 }.partition { |k, v| v.even? })
p (1..10).drop_while { |x| x < 5 }
