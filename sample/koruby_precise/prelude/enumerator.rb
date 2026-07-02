# Enumerator.product(*enums) — an enumerator over the Cartesian product.
class Enumerator
  def self.product(*enums)
    result = [[]]
    enums.each do |e|
      arr = e.to_a
      np = []
      result.each { |combo| arr.each { |x| np << (combo + [x]) } }
      result = np
    end
    if block_given?
      result.each { |c| yield c }
      nil
    else
      result.each
    end
  end
end

class Enumerator
  # Enumerator.produce(initial = nil) { |prev| ... } — an infinite enumerator of
  # initial, f(initial), f(f(initial)), …  With no initial the first value is
  # f(nil).  The block may raise StopIteration to terminate.
  def self.produce(*args, &block)
    raise ArgumentError, "tried to call produce without a block" unless block
    raise ArgumentError, "wrong number of arguments" if args.size > 1
    Enumerator.new do |y|
      cur = args.empty? ? block.call(nil) : args[0]
      loop do
        y << cur
        cur = block.call(cur)
      end
    end
  end
end
