# to_set(klass, *args) — deprecated in CRuby 4.0 but still honoured: build an
# instance of the given Set subclass instead of Set.  The builtin Array/Hash/Range
# #to_set take no argument, so wrap them for the argument form only.
module Enumerable
  private def __to_set_klass(args, &blk)
    warn "warning: passing arguments to Enumerable#to_set is deprecated"
    args[0].new(is_a?(Array) ? self : to_a, *args[1..-1], &blk)   # a Set subclass populates from an Array
  end
  def to_set(*args, &blk)
    return Set.new(self, &blk) if args.empty?
    __to_set_klass(args, &blk)
  end
end
[Array, Hash, Range].each do |k|
  k.class_eval do
    alias_method :__to_set_c, :to_set
    def to_set(*args, &blk)
      return __to_set_c(&blk) if args.empty?
      __to_set_klass(args, &blk)
    end
  end
end

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
  # in-place filters: collect the targets into a fresh array first (Set#to_a aliases
  # internal storage, so deleting while iterating it would skip elements).
  def delete_if; return to_enum(:delete_if) { size } unless block_given?; rm = []; each { |x| rm << x if yield(x) }; rm.each { |x| delete(x) }; self; end
  def keep_if; return to_enum(:keep_if) { size } unless block_given?; rm = []; each { |x| rm << x unless yield(x) }; rm.each { |x| delete(x) }; self; end
  def reject!; return to_enum(:reject!) { size } unless block_given?; n = size; delete_if { |x| yield(x) }; size == n ? nil : self; end
  def select!; return to_enum(:select!) { size } unless block_given?; n = size; keep_if { |x| yield(x) }; size == n ? nil : self; end
  alias filter! select!                    # CRuby: #filter! is the same UnboundMethod as #select!
  def collect!; return to_enum(:collect!) { size } unless block_given?; old = []; each { |x| old << x }; nw = old.map { |e| yield(e) }; old.each { |x| delete(x) }; nw.each { |x| self << x }; self; end
  alias map! collect!                      # CRuby: #map! and #collect! share one UnboundMethod
  def classify; return to_enum(:classify) { size } unless block_given?; h = {}; each { |e| k = yield(e); (h[k] ||= self.class.new) << e }; h; end
  # remove every element, returning self.
  def clear
    rm = []
    each { |x| rm << x }
    rm.each { |x| delete(x) }
    self
  end
  # replace self's contents with the given enumerable's, in place.
  def replace(enum)
    raise ArgumentError, "value must be enumerable" unless enum.respond_to?(:each)
    clear
    enum.each { |x| self << x }
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
    return to_enum(:divide) unless block_given?
    if func.arity == 2
      # Connected components of the directed graph u->v where func(u,v) is truthy.
      # CRuby 4.0 calls the block once for every ordered pair of *distinct*
      # elements; the groups are its strongly-connected components.
      els = to_a
      adj = Array.new(els.size) { [] }
      els.each_with_index do |u, i|
        els.each_with_index { |v, j| adj[i] << j if i != j && func.call(u, v) }
      end
      idx = Array.new(els.size, nil); low = Array.new(els.size, 0)
      on_stack = Array.new(els.size, false); stack = []; counter = 0; comps = []
      strongconnect = lambda do |v|
        idx[v] = counter; low[v] = counter; counter += 1
        stack.push(v); on_stack[v] = true
        adj[v].each do |w|
          if idx[w].nil?
            strongconnect.call(w); low[v] = low[v] < low[w] ? low[v] : low[w]
          elsif on_stack[w]
            low[v] = low[v] < idx[w] ? low[v] : idx[w]
          end
        end
        if low[v] == idx[v]
          comp = []
          loop do
            w = stack.pop; on_stack[w] = false; comp << els[w]
            break if w == v
          end
          comps << comp
        end
      end
      els.each_index { |i| strongconnect.call(i) if idx[i].nil? }
      self.class.new(comps.map { |g| self.class.new(g) })
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
  alias eql? ==                             # CRuby: #eql? is the same UnboundMethod as #==
  # pp hook for a self-referencing Set: the elements are elided, not re-entered.
  def pretty_print_cycle(q)
    q.text(empty? ? "Set[]" : "Set[...]")
  end
end
