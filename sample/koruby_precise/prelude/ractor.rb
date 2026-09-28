# Ractor on the green-thread scheduler: each Ractor is a Thread running its block
# with the Ractor as self.  There is no isolation — objects are not checked or
# moved between Ractors — so code that is Ractor-safe on CRuby runs the same here
# (the library "works inside a Ractor" tests), and make_shareable / shareable?
# keep CRuby's deep-freeze meaning.  Arguments are passed as CRuby does: a
# shareable object by reference, anything else as a deep copy.
class Ractor
  class Error < RuntimeError; end
  class RemoteError < Error
    attr_reader :ractor
    def initialize(msg = nil, ractor = nil) = (super(msg); @ractor = ractor)
  end
  class ClosedError < StopIteration; end
  class IsolationError < Error; end
  class MovedError < Error; end
  class UnsafeError < Error; end
  class MovedObject < BasicObject; end

  # A channel any Ractor can send to; only its creator receives (not enforced here).
  class Port
    def initialize
      @q = Thread::Queue.new
      @owner = Ractor.current
      @id = @owner.__send__(:__next_port_id)
    end
    # a ractor's default port: id 0, owned by that ractor (not the creating one)
    def self.__default_for(r) = allocate.tap { |pt| pt.__send__(:__init_default, r) }
    def __init_default(r) = (@q = Thread::Queue.new; @owner = r; @id = 0)
    private :__init_default
    def send(obj, move: false)
      raise ClosedError, "The port was already closed" if @q.closed?
      @q << Ractor.__copy_arg(obj)
      self
    end
    alias << send
    def receive
      raise ClosedError, "The port was already closed" if @q.closed? && @q.empty?
      v = @q.pop
      raise ClosedError, "The port was already closed" if v.nil? && @q.closed? && @q.empty?
      v
    end
    def close = (@q.close; nil)
    def closed? = @q.closed?
    def __ready? = !@q.empty?
    def inspect = "#<Ractor::Port to:##{@owner.instance_variable_get(:@id)} id:#{@id}>"
  end

  @__count = 0
  @__main = nil

  def self.__next_id = (@__count += 1)

  def self.new(*args, name: nil, &blk)
    raise ArgumentError, "must be called with a block" unless blk
    unless @__warned                                  # once, even when $VERBOSE = nil silenced it (CRuby)
      @__warned = true
      if !$VERBOSE.nil? && Warning[:experimental]
        loc = caller_locations(1, 1)&.first
        Warning.warn("#{loc&.path}:#{loc&.lineno}: warning: Ractor API is experimental and may change in future versions of Ruby.\n", category: :experimental)
      end
    end
    raise TypeError, "no implicit conversion of #{name.class} into String" unless name.nil? || name.is_a?(String)
    r = allocate
    r.__send__(:__init, name, args.map { |a| __copy_arg(a) }, blk)
    r
  end

  def self.__copy_arg(obj)
    return obj if shareable?(obj)
    Marshal.load(Marshal.dump(obj))
  rescue StandardError
    begin
      obj.dup
    rescue StandardError
      obj
    end
  end

  def self.main
    @__main ||= allocate.tap { |m| m.__send__(:__init_main) }
  end

  def self.current
    Thread.current.instance_variable_get(:@__ractor) || main
  end

  def self.main? = current.equal?(main)
  def self.count = 1 + Thread.list.count { |t| t.instance_variable_get(:@__ractor) && t.alive? }

  def self.receive = current.default_port.receive
  class << self
    alias recv receive
  end
  # `receive` inside the Ractor.new block (self is the ractor): the current one's
  private def receive = Ractor.receive
  private def recv = Ractor.receive

  # the first of `ports` with a value waiting: [port, value]
  def self.select(*ports)
    raise ArgumentError, "specify at least one port" if ports.empty?
    spins = 0
    while true                             # not `loop`: it would swallow ClosedError (a StopIteration)
      ports.each { |pt| return [pt, pt.receive] if pt.__ready? || pt.closed? }   # closed and empty: ClosedError
      # no multi-queue wait: back off so a select nobody answers does not burn a CPU
      (spins += 1) < 64 ? Thread.pass : sleep(0.001)
    end
  end

  def self.store_if_absent(key)
    s = current.__send__(:__store)
    s.key?(key) ? s[key] : (s[key] = yield(key))
  end

  def self.[](key) = current[key]
  def self.[]=(key, val)
    current[key] = val
  end

  # ---- shareability: deep frozen, or a Module / immediate / Ractor ----
  def self.shareable?(obj) = __shareable?(obj, {}.compare_by_identity)

  def self.__shareable?(obj, seen)
    case obj
    when nil, true, false, Integer, Float, Symbol, Module, Ractor, Ractor::Port then return true
    end
    return true if seen.key?(obj)
    return false unless obj.frozen?
    seen[obj] = true
    __refs(obj).all? { |o| __shareable?(o, seen) }
  end

  def self.make_shareable(obj, copy: false)
    obj = Marshal.load(Marshal.dump(obj)) if copy && !shareable?(obj)
    __freeze_deep(obj, {}.compare_by_identity)
    obj
  end

  def self.__freeze_deep(obj, seen)
    case obj
    when nil, true, false, Integer, Float, Symbol, Module, Ractor, Ractor::Port then return
    end
    return if seen.key?(obj)
    seen[obj] = true
    if obj.is_a?(Proc)
      unless __shareable?(obj.binding.receiver, {}.compare_by_identity)
        raise IsolationError, "Proc's self is not shareable: #{obj.inspect}"
      end
    end
    obj.freeze
    __refs(obj).each { |o| __freeze_deep(o, seen) }
  end

  def self.__refs(obj)
    refs = obj.instance_variables.map { |iv| obj.instance_variable_get(iv) }
    case obj
    when Array  then refs.concat(obj)
    when Hash   then obj.each { |k, v| refs << k << v }; refs << obj.default
    when Struct then refs.concat(obj.to_a)
    when Range  then refs << obj.begin << obj.end
    when Set    then refs.concat(obj.to_a)
    end
    refs
  end

  # CRuby rebinds the Proc's self to `self:`; koruby cannot, so the Proc keeps its
  # self (it is not checked) — the common use hands it to define_method, which
  # rebinds self anyway.
  def self.shareable_proc(self: nil, &blk) = blk.freeze
  def self.shareable_lambda(self: nil, &blk) = lambda(&blk).freeze

  # ---- instance ----
  attr_reader :name

  def __init(name, args, blk)
    @name = name
    @id = Ractor.__next_id
    @port = Port.__default_for(self)
    @port_seq = 0
    @store = {}
    @monitors = []
    me = self
    @thread = Thread.new do
      Thread.current.instance_variable_set(:@__ractor, me)
      begin
        v = me.instance_exec(*args, &blk)
      rescue Exception
        me.__send__(:__notify, :aborted)
        raise
      end
      me.__send__(:__notify, :exited)
      v
    end
  end
  private :__init

  def __init_main
    @name = nil
    @id = Ractor.__next_id
    @port = Port.__default_for(self)
    @port_seq = 0
    @store = {}
    @monitors = []
    @thread = Thread.main
  end
  private :__init_main

  def __next_port_id = (@port_seq += 1)
  def __store = @store
  def __notify(what) = @monitors.each { |pt| pt << what rescue nil }
  private :__store, :__notify, :__next_port_id

  def default_port = @port
  def monitor(port) = (@thread.alive? ? @monitors << port : port << (@thread.status.nil? ? :aborted : :exited); true)
  def unmonitor(port) = (@monitors.delete(port); nil)
  def close = (was = @port.closed?; @port.close; was)

  def [](key) = @store[key.to_sym]
  def []=(key, val)
    @store[key.to_sym] = val
  end

  def send(obj, move: false) = (@port.send(obj, move: move); self)
  alias << send

  def value
    raise Error, "The value was already taken" if @taken
    @taken = true
    Ractor.current.__send__(:__next_port_id)   # CRuby waits on a port of its own
    begin
      @thread.value
    rescue Exception => e
      raise RemoteError.new("thrown by remote Ractor.", self), cause: e
    end
  end
  def join = (value; self)


  def inspect
    st = @thread.equal?(Thread.main) ? "running" : (@thread.alive? ? "running" : "terminated")
    "#<Ractor:##{@id}#{@name ? " #{@name}" : ""} #{st}>"
  end
  alias to_s inspect
end

Ractor.main   # the main Ractor is #1
