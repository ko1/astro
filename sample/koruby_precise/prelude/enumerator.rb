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
