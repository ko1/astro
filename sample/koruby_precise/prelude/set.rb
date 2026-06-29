# Set#divide — partition into subsets.  1-arg block groups by its return value;
# 2-arg block forms connected components (a,b related iff func(a,b) && func(b,a)).
class Set
  # flatten nested Sets into a new Set; recursion raises ArgumentError.
  def flatten
    __flatten([])
  end
  def __flatten(seen)
    raise ArgumentError, "tried to flatten recursive Set" if seen.any? { |s| s.equal?(self) }
    seen.push(self)
    r = self.class.new
    each { |e| e.is_a?(Set) ? e.__flatten(seen).each { |x| r << x } : (r << e) }
    seen.pop
    r
  end
  # subtract — remove every element of the enumerable in place, returning self.
  def subtract(enum)
    raise ArgumentError, "value must be enumerable" unless enum.respond_to?(:each)
    enum.each { |x| delete(x) }
    self
  end
  def flatten!
    flat = flatten
    if flat == self
      nil
    else
      replace(flat)
      self
    end
  end
  def divide(&func)
    if func.arity == 2
      els = to_a
      parent = {}
      els.each { |e| parent[e] = e }
      root = lambda do |x|
        r = x
        r = parent[r] while parent[r] != r
        r
      end
      els.each do |a|
        els.each do |b|
          if !a.equal?(b) && func.call(a, b) && func.call(b, a)
            parent[root.call(a)] = root.call(b)
          end
        end
      end
      comps = {}
      els.each do |e|
        k = root.call(e)
        comps[k] = [] unless comps.key?(k)
        comps[k] << e
      end
      self.class.new(comps.values.map { |g| self.class.new(g) })
    else
      groups = {}
      each do |e|
        k = func.call(e)
        groups[k] = [] unless groups.key?(k)
        groups[k] << e
      end
      self.class.new(groups.values.map { |g| self.class.new(g) })
    end
  end
end
