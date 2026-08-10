# Minimal stubs for the process/thread/GC constants that mspec (and some specs)
# reference at load time.  koruby is single-threaded and non-forking; these give
# just enough surface to load mspec and run non-concurrent specs.  Real behavior
# is out of scope (Thread.new runs its block synchronously).

# Thread 本体 (new/join/value/pass/current/...) は C 実装 (builtins/thread.c、
# green thread M:1 — docs/io_design.md)。ここは Ruby で足りる補助だけ。
class ThreadGroup
  def initialize; @enclosed = false; end
  def enclose; @enclosed = true; self; end
  def enclosed?; @enclosed ? true : false; end
  def add(thread)
    raise ThreadError, "can't move to the enclosed thread group" if @enclosed
    g = thread.group
    raise ThreadError, "can't move from the enclosed thread group" if g && g.enclosed?
    thread.__set_group(self)
    self
  end
  def list; Thread.list.select { |t| t.group.equal?(self) }; end
end
ThreadGroup::Default = ThreadGroup.new

class Thread
  def self.exclusive; yield; end
  def self.exit; current.kill; end        # 明示定義 (無いと explicit-recv quirk で Kernel#exit に落ちる)
  def self.kill(th); th.kill; end
  def group; __group || ThreadGroup::Default; end
  # handle_interrupt 簡易版: :never を含む mask は区間全体を配送延期。
  # クラス別マスク / :on_blocking の精密な意味論は未対応 (自明の外)。
  def self.handle_interrupt(hash, &blk)
    raise ArgumentError, "block is needed" unless blk
    raise ArgumentError, "unknown mask signature" unless hash.is_a?(Hash) && !hash.empty?
    hash.each do |k, v|
      raise TypeError, "class or module required for rescue clause" unless k.is_a?(Module)
      raise ArgumentError, "unknown mask signature" unless [:immediate, :on_blocking, :never].include?(v)
    end
    if hash.values.include?(:never)
      current.__defer_ints_begin
      begin
        yield
      ensure
        current.__defer_ints_end
      end
    else
      yield
    end
  end
  def self.each_caller_location(*args, &blk)
    raise LocalJumpError, "no block given" unless blk
    (caller_locations(2) || []).each(&blk)
    nil
  end
  def self.report_on_exception; @roe.nil? ? true : @roe; end
  def self.report_on_exception=(v); @roe = v; end
end

# Queue / SizedQueue — Mutex + ConditionVariable (C 実装) の上の純 Ruby。
class Queue
  def initialize(enum = nil)
    @__items = []; @__mutex = Mutex.new; @__cond = ConditionVariable.new
    @__closed = false
    unless enum.nil?
      if enum.is_a?(Array)
        @__items.concat(enum)
      else
        raise TypeError, "can't convert #{enum.class} into Array" unless enum.respond_to?(:to_a)
        arr = enum.to_a
        raise TypeError, "can't convert #{enum.class} to Array (#{enum.class}#to_a gives #{arr.class})" unless arr.is_a?(Array)
        @__items.concat(arr)
      end
    end
  end
  def push(x)
    @__mutex.synchronize do
      raise ClosedQueueError, "queue closed" if @__closed
      @__items << x
      @__cond.signal
    end
    self
  end
  alias << push
  alias enq push
  def pop(non_block = false, timeout: nil)
    timeout = __q_tmo(timeout)
    @__mutex.synchronize do
      raise ArgumentError, "can't set a timeout if non_block is enabled" if timeout && non_block
      while @__items.empty?
        return nil if @__closed
        raise ThreadError, "queue empty" if non_block
        if timeout
          @__cond.wait(@__mutex, timeout)
          return @__items.shift unless @__items.empty?
          return nil                    # timed out (single-shot; clock は秒精度)
        end
        @__cond.wait(@__mutex)
      end
      @__items.shift
    end
  end
  alias shift pop
  alias deq pop
  def empty?; @__items.empty?; end
  def size; @__items.size; end
  alias length size
  def clear; @__mutex.synchronize { @__items.clear }; self; end
  def close
    @__mutex.synchronize { @__closed = true; @__cond.broadcast }
    self
  end
  def closed?; @__closed; end
  def num_waiting; @__cond.__num_waiting; end
  def __q_tmo(t)                      # CRuby: 入口で Float 化 (nil のみ「無期限」)
    return nil if t.nil?
    unless t.is_a?(Numeric)
      d = (t == true || t == false) ? t.inspect : t.class
      raise TypeError, "no implicit conversion of #{d} into Float"
    end
    t.to_f
  end
  private :__q_tmo
  def freeze; raise TypeError, "cannot freeze #{self}"; end
end

class SizedQueue < Queue
  def initialize(max)
    raise TypeError, "no implicit conversion from #{max.nil? ? 'nil' : max.class} to integer" unless max.is_a?(Numeric)
    max = max.to_i
    raise ArgumentError, "queue size must be positive" unless max > 0
    super()
    @__max = max
    @__cond_full = ConditionVariable.new
  end
  def num_waiting; @__cond.__num_waiting + @__cond_full.__num_waiting; end
  def max; @__max; end
  def max=(v)
    raise ArgumentError, "queue size must be positive" unless v.is_a?(Integer) && v > 0
    @__mutex.synchronize { @__max = v; @__cond_full.broadcast }
    v
  end
  def push(x, non_block = false, timeout: nil)
    timeout = __q_tmo(timeout)
    r = @__mutex.synchronize do
      raise ArgumentError, "can't set a timeout if non_block is enabled" if timeout && non_block
      raise ClosedQueueError, "queue closed" if @__closed
      while @__items.size >= @__max
        raise ThreadError, "queue full" if non_block
        if timeout
          @__cond_full.wait(@__mutex, timeout)
          raise ClosedQueueError, "queue closed" if @__closed
          break if @__items.size < @__max
          break :timeout
        end
        @__cond_full.wait(@__mutex)
        raise ClosedQueueError, "queue closed" if @__closed
      end
      if @__items.size < @__max
        @__items << x
        @__cond.signal
        nil
      else
        :timeout
      end
    end
    r == :timeout ? nil : self
  end
  alias << push
  alias enq push
  def pop(non_block = false, timeout: nil)
    timeout = __q_tmo(timeout)
    @__mutex.synchronize do
      raise ArgumentError, "can't set a timeout if non_block is enabled" if timeout && non_block
      while @__items.empty?
        return nil if @__closed
        raise ThreadError, "queue empty" if non_block
        if timeout
          @__cond.wait(@__mutex, timeout)
          break unless @__items.empty?
          return nil
        end
        @__cond.wait(@__mutex)
      end
      v = @__items.shift
      @__cond_full.signal
      v
    end
  end
  alias shift pop
  alias deq pop
  def close
    @__mutex.synchronize { @__closed = true; @__cond.broadcast; @__cond_full.broadcast }
    self
  end
end

# CRuby: これらは Thread:: の下が本体で toplevel は alias
Thread.const_set(:Mutex, Mutex)
Thread.const_set(:Queue, Queue)
Thread.const_set(:SizedQueue, SizedQueue)
Thread.const_set(:ConditionVariable, ConditionVariable)

module Process
  CLOCK_MONOTONIC = 1
  CLOCK_REALTIME = 0
  CLOCK_PROCESS_CPUTIME_ID = 2
  def self.pid; $$; end
  def self.ppid; 0; end
  def self.uid; 0; end
  def self.gid; 0; end
  def self.euid; 0; end
  def self.egid; 0; end
  def self.clock_gettime(_clk = CLOCK_MONOTONIC, unit = :float_second)
    t = Time.now.to_f
    case unit
    when :float_second then t
    when :float_millisecond then t * 1000.0
    when :float_microsecond then t * 1_000_000.0
    when :nanosecond then (t * 1_000_000_000).to_i
    when :millisecond then (t * 1000).to_i
    when :microsecond then (t * 1_000_000).to_i
    else (t * 1_000_000_000).to_i
    end
  end
  def self.times; Struct.new(:utime, :stime, :cutime, :cstime).new(0.0, 0.0, 0.0, 0.0); end
end

module GC
  def self.start(*); nil; end
  def self.enable; false; end
  def self.disable; false; end
  def self.stat(*); {}; end
  def self.count; 0; end
  def self.stress; false; end
  def self.stress=(v); v; end
end

module ObjectSpace
  def self.each_object(*)
    return 0 unless block_given?
    0
  end
  def self.count_objects(*); {}; end
  def self.garbage_collect(*); nil; end
  def self.define_finalizer(obj, proc = nil, &blk); 0; end     # no-op (GC finalizer 未対応)
  def self.undefine_finalizer(obj); obj; end
end

# 定義済みグローバル (CRuby 初期値)
$/ = "\n"        # input record separator
$\ = nil         # output record separator
$, = nil         # output field separator
$; = nil         # input field separator
$DEBUG = false
$VERBOSE = false
$stdin  = STDIN  if defined?(STDIN)
$stdout = STDOUT if defined?(STDOUT)
$stderr = STDERR if defined?(STDERR)

class IO
  READABLE = 1   # POLLIN
  PRIORITY = 2   # POLLPRI
  WRITABLE = 4   # POLLOUT
  def path; @__io_path; end
  def to_path; @__io_path; end
  def size; stat.size; end
  # io/wait: IO#wait(events_int|symbols…, timeout) — POLL blop 1 発
  def wait(*args)
    events = 0
    timeout = nil
    int_form = false
    args.each do |x|
      case x
      when Integer
        if events.zero? && !int_form && args.length >= 1 && x == args[0] && !x.between?(0, 7)
          timeout = x           # wait(10) 形: 大きい Integer は timeout と解釈しない (CRuby は events)
        end
        events |= x
        int_form = true
      when Float   then timeout = x
      when nil     then timeout = nil
      when Symbol
        case x
        when :read, :r, :readable        then events |= READABLE
        when :write, :w, :writable       then events |= WRITABLE
        when :read_write, :rw, :readable_writable then events |= (READABLE | WRITABLE)
        else raise ArgumentError, "unsupported mode: #{x}"
        end
      when Numeric then timeout = x
      end
    end
    events = READABLE if events.zero?
    r = __io_poll(events, timeout)
    return nil if r.zero?
    int_form ? r : self
  end
end

class File
  NULL = "/dev/null"
  LOCK_SH = 1; LOCK_EX = 2; LOCK_NB = 4; LOCK_UN = 8
  def flock(_op); 0; end       # 単一プロセス: no-op が正しい近似
  SEPARATOR = "/"
  ALT_SEPARATOR = nil
  PATH_SEPARATOR = ":"
  Separator = SEPARATOR
end

# ARGF: class 判別のためだけの最小 stub (CSV 等が ARGF.class を参照する)
class ARGFClass
  def argv; ARGV; end
  def filename; "-"; end
end
ARGF = ARGFClass.new

module ObjectSpace
  # WeakMap: 弱参照なしの機能 stub (weakref.rb 用)
  class WeakMap
    def initialize; @h = {}; end
    def [](k); @h[k]; end
    def []=(k, v); @h[k] = v; end
    def key?(k); @h.key?(k); end
    def delete(k); @h.delete(k); end
    def size; @h.size; end
  end
end

module Signal
  def self.trap(*); nil; end
  def self.signame(_n); nil; end
  def self.list; { "INT" => 2, "TERM" => 15, "KILL" => 9 }; end
end

module RbConfig
  CONFIG = {
    "host_os" => "linux", "host_cpu" => "x86_64", "arch" => "x86_64-linux",
    "ruby_install_name" => "koruby", "RUBY_INSTALL_NAME" => "koruby",
    "EXEEXT" => "", "bindir" => "/usr/bin", "rubylibdir" => "/usr/lib/ruby",
    "ruby_version" => RUBY_VERSION, "MAJOR" => RUBY_VERSION.split(".")[0],
    "MINOR" => RUBY_VERSION.split(".")[1], "TEENY" => RUBY_VERSION.split(".")[2],
  }
  def self.ruby; File.join(CONFIG["bindir"], CONFIG["ruby_install_name"] + CONFIG["EXEEXT"]); end
end

# at_exit: register a block to run (in reverse order) when the program ends.  The
# C main loop drains $__at_exit after the top-level program returns.
$__at_exit = []
def at_exit(&block)
  $__at_exit << block if block
  block
end

# pp / pretty_inspect — mspec's failure formatter calls #pretty_inspect; without
# it a genuine failure is reported as an error.  A plain inspect (+ newline, as pp
# does) is enough for spec messages.
class Object
  def pretty_inspect; inspect + "\n"; end
  def pretty_print(q); q.text(inspect); end
  def pretty_print_cycle(q); q.text(inspect); end
end

module Kernel
  # pp(*objs) — pretty-print each and return the argument(s) (Kernel#pp).
  def pp(*objs)
    objs.each { |obj| $stdout.write(obj.pretty_inspect) }
    objs.size <= 1 ? objs.first : objs
  end
  private :pp
end

module Process
  # The wait(2) status word behind $?.  `@raw` is the value waitpid filled in.
  class Status
    def initialize(pid, raw)
      @pid = pid
      @raw = raw
    end

    def pid = @pid
    def to_i = @raw
    def to_s = exitstatus ? "pid #{@pid} exit #{exitstatus}" : "pid #{@pid} SIG#{termsig}"
    def inspect = "#<Process::Status: #{to_s}>"
    def exited? = (@raw & 0x7f) == 0
    def exitstatus = exited? ? ((@raw >> 8) & 0xff) : nil
    def signaled? = ((@raw & 0x7f) + 1) >> 1 > 0
    def termsig = signaled? ? (@raw & 0x7f) : nil
    def stopped? = (@raw & 0xff) == 0x7f
    def stopsig = stopped? ? ((@raw >> 8) & 0xff) : nil
    def success? = exited? ? exitstatus == 0 : nil
    def coredump? = false
    def ==(other) = other.is_a?(Status) ? to_i == other.to_i : to_i == other
    def &(mask) = @raw & mask
    def >>(n) = @raw >> n
  end

  def self.__mkstatus(pid, raw) = Status.new(pid, raw)

  # The fork/exec primitives live on Object (builtins/process.c registers them
  # before this file is parsed); Process just names them.
  WNOHANG = 1
  WUNTRACED = 2
  def self.spawn(*a) = __spawn(*a)
  def self.wait(*a) = __waitpid(*a)
  def self.waitpid(*a) = __waitpid(*a)
  def self.wait2(*a) = __waitpid2(*a)
  def self.waitpid2(*a) = __waitpid2(*a)
  def self.kill(*a) = __kill(*a)
  def self.getpgid(*a) = __getpgid(*a)
  def self.getpgrp = __getpgid(0)
  def self.setpgid(pid, pgid) = __setpgid(pid, pgid)
end
