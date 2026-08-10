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

class Enumerator
  # External iteration (next / peek) of a generator-backed Enumerator
  # (to_enum / Enumerator.new, mode 3/4).  Its `each` body is not restartable —
  # it may have side effects — so the C fallback of re-driving from the start to
  # cursor+1 silently loses values.  Drive it with a Fiber instead, suspending at
  # every yield.  Eager and lazy enumerators keep the C path: lazy #each cannot
  # stream an infinite source, but its bounded re-drive can.
  alias_method :__c_next, :next
  alias_method :__c_peek, :peek
  alias_method :__c_next_values, :next_values
  alias_method :__c_peek_values, :peek_values
  alias_method :__c_rewind, :rewind

  def next
    return __c_next if __enum_mode < 3
    vals = __ext_next_values
    vals.size <= 1 ? vals[0] : vals
  end

  def peek
    return __c_peek if __enum_mode < 3
    vals = __ext_peek_values
    vals.size <= 1 ? vals[0] : vals
  end

  def next_values
    return __c_next_values if __enum_mode < 3
    __ext_next_values
  end

  def peek_values
    return __c_peek_values if __enum_mode < 3
    __ext_peek_values
  end

  def rewind
    @__ext_fiber = nil
    @__ext_started = false
    @__ext_buf = nil
    @__ext_have = false
    @__ext_done = false
    __c_rewind
  end

  private

  # The Fiber yields [args] for every element and returns nil when `each` ends,
  # so a nil resume value unambiguously means "exhausted".
  def __ext_start
    @__ext_fiber = Fiber.new do
      each { |*args| Fiber.yield([args]) }
      nil
    end
    @__ext_started = true
    @__ext_buf = nil
    @__ext_have = false
    @__ext_done = false
  end

  def __ext_peek_values
    __ext_start unless @__ext_started
    unless @__ext_have
      if @__ext_done
        @__ext_buf = nil
      else
        r = @__ext_fiber.resume
        if r.nil?
          @__ext_done = true
          @__ext_buf = nil
        else
          @__ext_buf = r[0]
        end
      end
      @__ext_have = true
    end
    raise StopIteration, "iteration reached an end" if @__ext_buf.nil?
    @__ext_buf
  end

  def __ext_next_values
    v = __ext_peek_values
    @__ext_have = false
    @__ext_buf = nil
    v
  end
end
