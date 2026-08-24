class Enumerator
  # An Enumerator subclass written in Ruby (Enumerator::Chain / ::Product below,
  # or a user's) keeps its state in ivars, not in the C enumerator struct that
  # Enumerator's own #to_a / #map / #first / … read — reading that struct off a
  # plain object crashes.  In CRuby those methods are Enumerable's anyway (they
  # just drive #each), so hand every Ruby-defined subclass the Enumerable
  # implementations, inserted ahead of Enumerator's C ones.
  module EachDriven
    Enumerable.instance_methods(false).each do |m|
      define_method(m, Enumerable.instance_method(m))
    end
  end

  def self.inherited(sub)
    sub.include(EachDriven)
    super
  end

  def +(other)
    Enumerator::Chain.new(self, other)
  end

  # Enumerator::Chain — the concatenation of several enumerables.
  class Chain < Enumerator
    def initialize(*enums)
      raise FrozenError, "can't modify frozen #{self.class}" if frozen?
      @__enums = enums
      @__pos = -1          # index last iterated; -1 = never iterated
      self
    end
    private :initialize

    def each(&block)
      return to_enum(:each) { size } unless block
      raise ArgumentError, "uninitialized chain" unless @__enums
      @__enums.each_with_index do |e, i|
        @__pos = i
        e.each { |*x| block.call(*x) }
      end
      self
    end

    # Rewinds the constituents that have actually been iterated, in reverse
    # order, and only those that respond to #rewind.
    def rewind
      while @__pos >= 0 && @__pos < @__enums.size
        e = @__enums[@__pos]
        e.rewind if e.respond_to?(:rewind)
        @__pos -= 1
      end
      self
    end

    # The sum of the constituents' sizes, short-circuiting on the first nil or
    # infinite one (later constituents are not asked at all).
    def size
      total = 0
      @__enums.each do |e|
        return nil unless e.respond_to?(:size)
        s = e.size
        return s if s.nil? || (s.is_a?(Float) && s.infinite?)
        total += s
      end
      total
    end

    def inspect
      return "#<#{self.class}: uninitialized>" unless @__enums
      return "#<#{self.class}: ...>" if @__inspecting
      @__inspecting = true
      begin
        "#<#{self.class}: [#{@__enums.map { |e| e.inspect }.join(', ')}]>"
      ensure
        @__inspecting = false
      end
    end
    alias_method :to_s, :inspect
  end

  # Enumerator::Product — the Cartesian product of several enumerables.  The
  # last enumerable varies fastest.  Constituents are driven with #each_entry.
  class Product < Enumerator
    def initialize(*enums)
      raise FrozenError, "can't modify frozen #{self.class}" if frozen?
      @__enums = enums
      self
    end
    private :initialize

    def initialize_copy(other)
      return self if other.equal?(self)
      raise FrozenError, "can't modify frozen #{self.class}" if frozen?
      raise TypeError, "initialize_copy should take same class object" unless other.class == self.class
      oe = other.instance_variable_get(:@__enums)
      raise ArgumentError, "uninitialized product" unless oe
      @__enums = oe
      self
    end
    private :initialize_copy

    def each(&block)
      return to_enum(:each) { size } unless block
      raise ArgumentError, "uninitialized product" unless @__enums
      __each_from(0, [], block)
      self
    end

    def rewind
      @__enums.each { |e| e.rewind if e.respond_to?(:rewind) }
      self
    end

    # The product of the constituents' sizes; nil unless every one of them
    # reports an Integer (or one reports Infinity, which wins).
    def size
      total = 1
      @__enums.each do |e|
        return nil unless e.respond_to?(:size)
        s = e.size
        return s if s.is_a?(Float) && s.infinite?
        return nil unless s.is_a?(Integer)
        total *= s
      end
      total
    end

    def inspect
      return "#<#{self.class}: uninitialized>" unless @__enums
      return "#<#{self.class}: ...>" if @__inspecting
      @__inspecting = true
      begin
        "#<#{self.class}: [#{@__enums.map { |e| e.inspect }.join(', ')}]>"
      ensure
        @__inspecting = false
      end
    end
    alias_method :to_s, :inspect

    private

    def __each_from(i, acc, block)
      if i == @__enums.size
        block.call(acc)
      else
        @__enums[i].each_entry { |x| __each_from(i + 1, acc + [x], block) }
      end
    end
  end

  # Enumerator.product(*enums) — an enumerator over the Cartesian product.
  def self.product(*enums, **kw, &block)
    raise ArgumentError, "unknown keywords: #{kw.keys.map { |k| k.inspect }.join(', ')}" unless kw.empty?
    e = Product.new(*enums)
    return e unless block
    e.each { |c| block.call(c) }
    nil
  end
end

class Enumerator
  # Enumerator.produce(initial = nil) { |prev| ... } — an infinite enumerator of
  # initial, f(initial), f(f(initial)), …  With no initial the first value is
  # f(nil).  The block may raise StopIteration to terminate.
  def self.produce(*args, **opts, &block)
    raise ArgumentError, "no block given" unless block
    size = opts.key?(:size) ? opts.delete(:size) : Float::INFINITY
    raise ArgumentError, "unknown keywords: #{opts.keys.join(', ')}" unless opts.empty?
    raise ArgumentError, "wrong number of arguments" if args.size > 1
    Enumerator.new(size) do |y|
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
    return __c_next if __enum_mode < 3 && !@__ext_fed
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
    @__ext_feed = nil
    @__ext_fed = false
    __c_rewind
  end

  # The value the source's `yield` returns on the NEXT step.  Our external
  # iteration is a Fiber, so it is simply the next #resume argument.
  def feed(value)
    raise TypeError, "feed value already set" if @__ext_fed
    @__ext_fed = true
    @__ext_feed = value
    nil
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
    @__ext_fresh = true
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
        # The first resume only starts the source; the value fed before that is
        # delivered by the NEXT resume (that is the one the first `yield`
        # returns from), so it is not consumed here.
        fed = nil
        unless @__ext_fresh
          fed = @__ext_feed if @__ext_fed
          @__ext_fed = false
          @__ext_feed = nil
        end
        @__ext_fresh = false
        r = @__ext_fiber.resume(fed)
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

class Enumerator
  # Remember what an enum_for/to_enum enumerator was built from, so
  # `enum.each(extra...)` can re-drive `receiver.meth(*args, *extra)` (CRuby).
  def __set_source(recv, meth, args)
    @__src_recv = recv
    @__src_meth = meth
    @__src_args = args
    self
  end

  def each(*extra, &block)
    return self if block.nil? && extra.empty?
    if !extra.empty? && defined?(@__src_recv)
      return @__src_recv.send(@__src_meth, *@__src_args, *extra, &block) if block
      recv, meth, args = @__src_recv, @__src_meth, @__src_args + extra
      return Enumerator.new { |y| recv.send(meth, *args) { |*vs| y << (vs.size <= 1 ? vs[0] : vs) } }
    end
    return __each_orig(&block) if block
    self
  end
end

class Enumerator::Lazy
  # Lazy#initialize(receiver[, size]) { |yielder, *values| ... } — the block is a
  # TRANSFORM: it is handed a yielder plus each source value and decides what to
  # emit.  koruby's lazy enumerators are generator-backed, so fold the pair into
  # one generator block and hand it to Enumerator#initialize.
  def initialize(receiver, size = nil, &block)
    raise ArgumentError, "tried to call lazy new without a block" unless block
    raise FrozenError.new("can't modify frozen #{self.class}", receiver: self) if frozen?
    @__lazy_size = size
    super() do |y|
      receiver.each { |*values| block.call(y, *values) }
    end
    self
  end
  private :initialize

  # Only a Lazy built through #initialize carries an explicit size; every
  # derived lazy keeps the builtin behaviour.
  def size
    return super unless defined?(@__lazy_size)
    @__lazy_size.is_a?(Proc) ? @__lazy_size.call : @__lazy_size
  end

  # #eager — 以降を非 lazy に戻す (列挙内容は同じ)。koruby の lazy は
  # Enumerator の mode で表すので、eager な Enumerator を作り直す。
  def eager
    src = self
    Enumerator.new { |y| src.each { |*a| y.yield(*a) } }
  end
end
