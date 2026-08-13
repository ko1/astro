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
    t = __clock_gettime
    case unit
    when :float_second, nil then t          # nil は既定 (:float_second)
    when :float_millisecond then t * 1000.0
    when :float_microsecond then t * 1_000_000.0
    when :second then t.to_i
    when :millisecond then (t * 1000).to_i
    when :microsecond then (t * 1_000_000).to_i
    when :nanosecond then (t * 1_000_000_000).to_i
    else raise ArgumentError, "unexpected unit: #{unit}"
    end
  end

  PRIO_PROCESS = 0
  PRIO_PGRP    = 1
  PRIO_USER    = 2
  def self.getpriority(which, who) = __getpriority(__int_arg(which), __int_arg(who))
  def self.setpriority(which, who, prio) = __setpriority(__int_arg(which), __int_arg(who), __int_arg(prio))
  def self.__int_arg(v)
    return v if v.is_a?(Integer)
    raise TypeError, "no implicit conversion of #{v.class} into Integer" unless v.respond_to?(:to_int)
    n = v.to_int
    raise TypeError, "can't convert #{v.class} to Integer" unless n.is_a?(Integer)
    n
  end
  private_class_method :__int_arg
  def self.times; Struct.new(:utime, :stime, :cutime, :cstime).new(0.0, 0.0, 0.0, 0.0); end

  def self.getrlimit(resource) = __getrlimit(__as_rlimit_int(resource))
  def self.setrlimit(resource, soft, hard = nil)
    r = __as_rlimit_int(resource)
    s = __as_rlimit_int(soft)
    __setrlimit(r, s, hard.nil? ? s : __as_rlimit_int(hard))
  end

  def self.__as_rlimit_int(v)
    return v if v.is_a?(Integer)
    # A Symbol/String names a resource (:CORE / "CORE") or a special limit value
    # (:INFINITY).  __rlimit_table keys are "RLIMIT_CORE", "INFINITY", …
    if v.is_a?(Symbol) || v.is_a?(String)
      key = v.to_s
      tbl = __rlimit_table
      return tbl[key] if tbl.key?(key)
      rk = "RLIMIT_#{key}"
      return tbl[rk] if tbl.key?(rk)
      raise ArgumentError, "unknown resource name - #{key}"
    end
    raise TypeError, "no implicit conversion of #{v.nil? ? 'nil' : v.class} into Integer" unless v.respond_to?(:to_int)
    i = v.to_int
    raise TypeError, "can't convert #{v.class} to Integer (#{v.class}#to_int gives #{i.class})" unless i.is_a?(Integer)
    i
  end
  private_class_method :__as_rlimit_int

  def self.clock_getres(_clk = CLOCK_MONOTONIC, unit = :float_second)
    ns = 1
    case unit
    when :float_second      then ns / 1_000_000_000.0
    when :float_millisecond then ns / 1_000_000.0
    when :float_microsecond then ns / 1000.0
    when :millisecond       then 0
    when :microsecond       then 0
    else ns
    end
  end

  def self.argv0 = $0

  # Reap the child in the background; the thread's value is its Process::Status.
  def self.detach(pid)
    Thread.new(pid) { |p| Process.wait2(p)[1] }
  end

  module Sys
    def self.getuid = Process.uid
    def self.geteuid = Process.euid
    def self.getgid = Process.gid
    def self.getegid = Process.egid
  end
end

# Resource limits.  The RLIMIT_* numbers are the platform's, so the constant set
# matches what getrlimit(2) actually accepts here.  Done at top level: a builtin
# called from a module *body* (self = the module) does not resolve, only from
# inside a method.
__rlimit_table.each do |name, num|
  if name == "INFINITY"
    Process.const_set(:RLIM_INFINITY, num)
    Process.const_set(:RLIM_SAVED_MAX, num)
    Process.const_set(:RLIM_SAVED_CUR, num)
  else
    Process.const_set(name, num)
  end
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
  # File#lstat — stat の symlink 版。開いているのはファイル本体なので、
  # 開いたときのパスを lstat し直す (パスがなければ #stat と同じ)。
  def lstat
    @__io_path ? File.lstat(@__io_path) : stat
  end
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

  # File.path(obj) — the path String an object names: #to_path if it has one,
  # else the String itself (#to_str-coerced).  Pathname is built on this.
  def self.path(obj)
    return obj if obj.is_a?(String)
    return obj.to_path.to_str if obj.respond_to?(:to_path)
    return obj.to_str if obj.respond_to?(:to_str)
    raise TypeError, "no implicit conversion of #{obj.nil? ? 'nil' : obj.class} into String"
  end
end

# ARGF — the concatenation of the files named in ARGV, or stdin when ARGV is
# empty.  `ARGF` is an instance of this class; specs (and mspec's argf helper)
# also build their own with `ARGF.class.new(*filenames)`.
#
# Opening is lazy but eager enough that #file / #filename can name the first
# file before anything is read, and the *last* file stays reported after the
# stream is exhausted — both are behaviours the specs pin down.  A file is only
# left behind on the read *after* it hit EOF, so #filename still names it while
# its last line is being processed.
class ARGFClass
  include Enumerable

  def initialize(*argv)
    @argv = argv
    @current = nil          # IO of the file being read (kept after close)
    @current_name = nil
    @opened_any = false
    @advance = false        # current file is done; open the next one on demand
    @lineno = 0             # cumulative across files ($.)
    @file_lineno = 0        # lines read from the current file (for #rewind)
    @binmode = false
  end

  attr_reader :argv

  def to_s; "ARGF"; end
  alias_method :inspect, :to_s

  # --- stream plumbing -----------------------------------------------------

  def __next_file
    if @argv.empty?
      return false if @opened_any            # ARGV exhausted (or stdin already used)
      @current = STDIN
      @current_name = "-"
    else
      @current_name = @argv.shift.to_s
      @current = @current_name == "-" ? STDIN : File.open(@current_name, @binmode ? "rb" : "r")
    end
    @opened_any = true
    @advance = false
    @file_lineno = 0
    $FILENAME = @current_name
    true
  end
  private :__next_file

  def __finish_current
    @current.close if @current && !@current.equal?(STDIN) && !@current.closed?
    @advance = true
  end
  private :__finish_current

  # The IO to read from, skipping over files already at EOF; nil when the whole
  # stream is exhausted.
  def __stream
    loop do
      if @current.nil? || @advance
        return nil unless __next_file
      end
      return @current unless @current.closed? || @current.eof?
      __finish_current
    end
  end
  private :__stream

  # --- current file --------------------------------------------------------

  def file
    __next_file if @current.nil?
    @current
  end

  def filename
    __next_file if @current.nil?
    @current_name
  end
  alias_method :path, :filename

  def to_io; file; end

  def fileno
    io = file
    raise ArgumentError, "closed stream" if io.nil? || io.closed?
    io.fileno
  end
  alias_method :to_i, :fileno

  def closed?; file.closed?; end

  def close
    io = file
    io.close if io && !io.equal?(STDIN) && !io.closed?
    @advance = true
    self
  end

  def skip
    return self if @current.nil?             # nothing processed yet
    __finish_current
    self
  end

  def eof?
    io = file
    raise IOError, "stream closed" if io.nil? || io.closed?
    io.eof?
  end
  alias_method :eof, :eof?

  def rewind
    io = file
    raise ArgumentError, "no stream to rewind" if io.nil?
    @lineno -= @file_lineno
    @file_lineno = 0
    io.rewind
    0
  end

  def pos
    io = file
    raise ArgumentError, "closed stream" if io.nil? || io.closed?
    io.pos
  end
  alias_method :tell, :pos

  def pos=(n); file.pos = n; end
  def seek(*args); file.seek(*args); end

  def binmode; @binmode = true; @current.binmode if @current && !@current.closed?; self; end
  def binmode?; @binmode; end

  def set_encoding(*args); file.set_encoding(*args); self; end
  def external_encoding; @current ? file.external_encoding : Encoding.default_external; end
  def internal_encoding; @current ? file.internal_encoding : Encoding.default_internal; end

  def lineno; @lineno; end
  def lineno=(n); @lineno = n; $. = n; n; end

  # --- reading -------------------------------------------------------------

  def gets(*args)
    loop do
      io = __stream
      return nil if io.nil?
      line = io.gets(*args)
      if line.nil?
        __finish_current
        next
      end
      @lineno += 1
      @file_lineno += 1
      $. = @lineno
      return line
    end
  end

  def readline(*args)
    line = gets(*args)
    raise EOFError, "end of file reached" if line.nil?
    line
  end

  def each_line(*args, &blk)
    return to_enum(:each_line, *args) unless blk
    while (line = gets(*args))
      blk.call(line)
    end
    self
  end
  alias_method :each, :each_line
  alias_method :lines, :each_line

  def readlines(*args)
    r = []
    while (line = gets(*args))
      r << line
    end
    r
  end
  alias_method :to_a, :readlines

  def read(length = nil, buffer = nil)
    if length.nil?
      res = +""
      while (io = __stream)
        res << io.read.to_s
        __finish_current
      end
      return buffer.replace(res) if buffer
      return res
    end
    raise ArgumentError, "negative length #{length} given" if length < 0
    return (buffer ? buffer.replace("") : "") if length == 0
    res = nil
    while length > 0
      io = __stream
      break if io.nil?
      chunk = io.read(length)
      if chunk.nil? || chunk.empty?
        __finish_current
        next
      end
      res = res.nil? ? chunk.dup : (res + chunk)
      length -= chunk.bytesize
    end
    return buffer.replace(res || "") if buffer
    res
  end

  # readpartial / read_nonblock do not silently cross a file boundary: at the
  # end of one file they move on and read from the next, and only raise
  # EOFError once every file is exhausted.
  def __partial(meth, maxlen, buffer, **kw)
    loop do
      io = __stream
      raise EOFError, "end of file reached" if io.nil?
      begin
        return buffer ? io.send(meth, maxlen, buffer, **kw) : io.send(meth, maxlen, **kw)
      rescue EOFError
        __finish_current
      end
    end
  end
  private :__partial

  def readpartial(maxlen, buffer = nil); __partial(:readpartial, maxlen, buffer); end

  def read_nonblock(maxlen, buffer = nil, exception: true)
    __partial(:read_nonblock, maxlen, buffer, exception: exception)
  end

  def getc
    loop do
      io = __stream
      return nil if io.nil?
      ch = io.getc
      return ch unless ch.nil?
      __finish_current
    end
  end

  def readchar
    ch = getc
    raise EOFError, "end of file reached" if ch.nil?
    ch
  end

  def getbyte
    loop do
      io = __stream
      return nil if io.nil?
      b = io.getbyte
      return b unless b.nil?
      __finish_current
    end
  end

  def readbyte
    b = getbyte
    raise EOFError, "end of file reached" if b.nil?
    b
  end

  def each_char(&blk)
    return to_enum(:each_char) unless blk
    while (ch = getc)
      blk.call(ch)
    end
    self
  end
  alias_method :chars, :each_char

  def each_byte(&blk)
    return to_enum(:each_byte) unless blk
    while (b = getbyte)
      blk.call(b)
    end
    self
  end
  alias_method :bytes, :each_byte

  def each_codepoint(&blk)
    return to_enum(:each_codepoint) unless blk
    each_char { |ch| blk.call(ch.ord) }
    self
  end
  alias_method :codepoints, :each_codepoint

  def write(*args); STDOUT.write(*args); end
  def print(*args); STDOUT.print(*args); end
  def printf(*args); STDOUT.printf(*args); end
  def putc(c); STDOUT.putc(c); end
  def puts(*args); STDOUT.puts(*args); end

  def inplace_mode; nil; end
  def inplace_mode=(_ext); self; end
end
# CRuby names this class "ARGF.class" (it is not reachable as a constant there).
class << ARGFClass
  def name; "ARGF.class"; end
  alias_method :to_s, :name
  alias_method :inspect, :name
end
ARGF = ARGFClass.new(*ARGV)

module ObjectSpace
  # WeakMap / WeakKeyMap — koruby's GC has no weak edges, so entries are held
  # strongly.  Everything else (comparison semantics, which keys are accepted,
  # the API surface) follows CRuby; only "the entry disappears once the key is
  # collected" is missing.
  class WeakMap
    include Enumerable

    def initialize; @h = {}.compare_by_identity; end   # WeakMap compares keys by identity
    def [](k); @h[k]; end
    def []=(k, v); @h[k] = v; v; end
    def include?(k); @h.key?(k); end
    alias_method :key?, :include?
    alias_method :member?, :include?
    def delete(k, &blk)
      return @h.delete(k) if @h.key?(k)
      blk ? blk.call(k) : nil
    end
    def size; @h.size; end
    alias_method :length, :size
    def keys; @h.keys; end
    def values; @h.values; end

    # `each` on an empty map returns self even without a block; with entries and
    # no block it is a LocalJumpError (CRuby yields eagerly, it is not an
    # Enumerator-returning method).
    def each(&blk)
      return self if @h.empty? && blk.nil?
      raise LocalJumpError, "no block given (yield)" if blk.nil?
      @h.each { |k, v| blk.call(k, v) }
      self
    end
    alias_method :each_pair, :each

    def each_key(&blk)
      return self if @h.empty? && blk.nil?
      raise LocalJumpError, "no block given (yield)" if blk.nil?
      @h.each_key { |k| blk.call(k) }
      self
    end

    def each_value(&blk)
      return self if @h.empty? && blk.nil?
      raise LocalJumpError, "no block given (yield)" if blk.nil?
      @h.each_value { |v| blk.call(v) }
      self
    end

    def inspect
      body = @h.map { |k, v| "#{ObjectSpace.__any_to_s(k)} => #{ObjectSpace.__any_to_s(v)}" }.join(", ")
      format("#<ObjectSpace::WeakMap:0x%016x%s>", object_id, body.empty? ? "" : ": #{body}")
    end
  end

  # `#<Class:0xaddr>` for any object, including a BasicObject (which has no
  # #inspect / #class to call).
  def self.__any_to_s(o)
    cls = Object.instance_method(:class).bind_call(o) rescue nil
    id  = Object.instance_method(:object_id).bind_call(o) rescue 0
    format("#<%s:0x%016x>", cls ? cls.name : "BasicObject", id)
  end

  class WeakKeyMap
    def initialize; @h = {}; end

    def [](key)
      return nil unless __collectable?(key)
      e = @h[key]
      e && e[1]
    end

    # The key object itself is stored (Hash#[]= would dup+freeze a String key,
    # which #getkey must not observe), so entries are [key, value] pairs.
    def []=(key, value)
      raise ArgumentError, "WeakKeyMap keys must be garbage collectable" unless __collectable?(key)
      @h[key] = [key, value]
      value
    end

    def key?(key)
      return false unless __collectable?(key)
      @h.key?(key)
    end

    def getkey(key)
      return nil unless __collectable?(key)
      e = @h[key]
      e && e[0]
    end

    def delete(key, &blk)
      if __collectable?(key) && @h.key?(key)
        @h.delete(key)[1]
      elsif blk
        blk.call(key)
      end
    end

    def clear; @h.clear; self; end
    # No #size/#length: CRuby's WeakKeyMap exposes the count only via #inspect.
    def inspect; format("#<ObjectSpace::WeakKeyMap:0x%016x size=%d>", object_id, @h.size); end

    private

    # Integers / Floats / Symbols / true / false / nil are never collected, so
    # CRuby refuses them as keys and reports them as absent on reads.  Tested
    # with Module#=== so nothing is dispatched to the key itself (a BasicObject
    # key must fail on #hash, not on #nil?).
    def __collectable?(key)
      !(Integer === key || Float === key || Symbol === key ||
        NilClass === key || TrueClass === key || FalseClass === key)
    end
  end
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
  def self.last_status = $?
  def self.setpgid(pid, pgid) = __setpgid(pid, pgid)
end

module Signal
  # Signals are blocked process-wide and reaped at interpreter check points
  # (builtins/process.c); this is where the policy lives.  Trapping a signal
  # also blocks it, so INT/QUIT — left unblocked at startup so Ctrl-C stays
  # prompt — become koruby-delivered as soon as a program traps them.
  @@handlers = {}

  def self.trap(sig, command = nil, &blk)
    n = signo(sig)
    raise ArgumentError, "unsupported signal '#{sig}'" if n.nil?
    key = signame(n) || sig.to_s
    prev = @@handlers[key]
    h = command.is_a?(Symbol) ? command.to_s : (command || blk)   # :SIG_DFL / :IGNORE spell the same handlers
    @@handlers[key] = h
    if h.is_a?(String) && (h == "IGNORE" || h == "SIG_IGN")
      __signal_trap(sig, "IGNORE")     # a blocked+ignored signal is discarded by the kernel
      __signal_block(n, false)
    else
      __signal_trap(sig, "DEFAULT")    # undo a previous SIG_IGN; blocked, so it just stays pending
      __signal_block(n, true)          # deliver it ourselves at the next check point
    end
    prev.nil? ? "DEFAULT" : prev
  end

  # Called from the interpreter with a signal number just reaped from the
  # pending set.  Runs the trap handler, or raises for the default disposition.
  def self.__deliver(signo)
    case (h = @@handlers[signame(signo)])
    when "IGNORE", "SIG_IGN" then nil
    when "EXIT"              then exit(0)
    when nil, "DEFAULT", "SIG_DFL"
      raise(signo == Signal.list["INT"] ? Interrupt.new : SignalException.new(signo))
    else
      h.respond_to?(:call) ? h.call(signo) : nil
    end
  end

  def self.signame(signo) = __signal_signame(signo)
  def self.signo(sig) = sig.is_a?(Integer) ? sig : __signal_signo(sig)

  def self.list
    h = {}
    (1..31).each { |n| (nm = __signal_signame(n)) && h[nm] = n }
    h["EXIT"] = 0
    h
  end
end

# SignalException / Interrupt carry the signal they stand for.  Both classes are
# created on the C side; this gives them their #signo / #signm behaviour.
class SignalException < Exception
  attr_reader :signo, :signm

  def initialize(sig = nil, msg = nil)
    if sig.nil?
      @signo = nil
      @signm = self.class.name
    elsif sig.is_a?(Integer)
      nm = Signal.signame(sig)
      raise ArgumentError, "invalid signal number #{sig}" if nm.nil?
      @signo = sig
      @signm = msg || "SIG#{nm}"
    elsif sig.is_a?(String) || sig.is_a?(Symbol)
      raise ArgumentError, "wrong number of arguments (given 2, expected 1)" unless msg.nil?
      nm = sig.to_s.sub(/\ASIG/, "")
      n = Signal.list[nm]
      raise ArgumentError, "unsupported name '#{sig}'" if n.nil?
      @signo = n
      @signm = "SIG#{nm}"
    else
      raise ArgumentError, "bad signal type #{sig.class}"
    end
    super(@signm)
  end

  def message = @signm
  def to_s = @signm
end

class Interrupt < SignalException
  def initialize(msg = nil)
    super(Signal.list["INT"], msg || "Interrupt")
  end
end

module Kernel
  def trap(sig, command = nil, &blk) = Signal.trap(sig, command, &blk)
  module_function :trap

  # test(cmd, file[, file2]) — the file predicates, spelled as a character.
  def test(cmd, file1, file2 = nil) = __process_test(cmd, File.path(file1), file2 && File.path(file2))
  module_function :test

  # $-variable tracing: koruby has no hook on global assignment, so the
  # registration is recorded (and #untrace_var returns it) but never fires.
  def trace_var(name, cmd = nil, &blk)
    (($__traced_vars ||= {})[name.to_sym] ||= []) << (cmd || blk)
    nil
  end
  # Returns the removed commands (CRuby hands back the list it dropped).
  def untrace_var(name, cmd = nil)
    h = ($__traced_vars ||= {})
    k = name.to_sym
    return nil unless h.key?(k)
    if cmd
      h[k].delete(cmd)
      [cmd]
    else
      h.delete(k)
    end
  end
  module_function :trace_var, :untrace_var
end

class Object
  # The Method object for a singleton method, or NameError when the method is
  # not defined on the singleton class itself.
  def singleton_method(name)
    sc = singleton_class
    unless sc.instance_methods(false).include?(name.to_sym) ||
           sc.private_instance_methods(false).include?(name.to_sym)
      raise NameError.new("undefined singleton method '#{name}' for #{inspect}", name.to_sym)
    end
    method(name)
  end
end

# File / FileTest predicates on top of the __process_test character commands
# (the same primitive Kernel#test uses) and __mode_bits.
class File
  class << self
    def ftype(path)
      path = File.path(path)
      m = __mode_bits(path, false)
      raise Errno::ENOENT.new(nil, nil), "No such file or directory - #{path}" if m.nil?
      mode = m[0]
      case mode & 0o170000
      when 0o140000 then "socket"
      when 0o120000 then "link"
      when 0o100000 then "file"
      when 0o060000 then "blockSpecial"
      when 0o040000 then "directory"
      when 0o020000 then "characterSpecial"
      when 0o010000 then "fifo"
      else "unknown"
      end
    end

    def empty?(path) = __process_test("z".ord, File.path(path), nil)
    def identical?(a, b)
      __process_test("-".ord, File.path(a), File.path(b))
    rescue Errno::ENOENT, SystemCallError
      false
    end
    def pipe?(path)      = __process_test("p".ord, File.path(path), nil)
    def socket?(path)    = __process_test("S".ord, File.path(path), nil)
    def blockdev?(path)  = __process_test("b".ord, File.path(path), nil)
    def chardev?(path)   = __process_test("c".ord, File.path(path), nil)
    def sticky?(path)    = __process_test("k".ord, File.path(path), nil)
    def setuid?(path)    = __process_test("u".ord, File.path(path), nil)
    def setgid?(path)    = __process_test("g".ord, File.path(path), nil)
    def owned?(path)     = __process_test("o".ord, File.path(path), nil)
    def grpowned?(path)  = __process_test("G".ord, File.path(path), nil)
    # The _real? family uses real (not effective) ids; with no setuid in play
    # access(2) already answers with the real ids' rights on this runtime.
    def readable_real?(path)   = __process_test("r".ord, File.path(path), nil)
    def writable_real?(path)   = __process_test("w".ord, File.path(path), nil)
    def executable_real?(path) = __process_test("x".ord, File.path(path), nil)

    def world_readable?(path)
      m = __mode_bits(File.path(path), true)
      (m && (m[0] & 0o004) != 0) ? (m[0] & 0o777) : nil
    end
    def world_writable?(path)
      m = __mode_bits(File.path(path), true)
      (m && (m[0] & 0o002) != 0) ? (m[0] & 0o777) : nil
    end

    def utime(atime, mtime, *paths)
      __utime(atime&.to_f, mtime&.to_f, true, *paths)
    end
    def lutime(atime, mtime, *paths)
      __utime(atime&.to_f, mtime&.to_f, false, *paths)
    end
  end

  # Instance-side chown/chmod go through the path (koruby IOs keep their path).
  def chown(owner, group)
    File.chown(owner, group, path)
    0
  end
  def chmod(mode)
    File.chmod(mode, path)
    0
  end
end

module FileTest
  %i[empty? identical? pipe? socket? blockdev? chardev? sticky? setuid? setgid?
     owned? grpowned? readable_real? writable_real? executable_real?
     world_readable? world_writable?].each do |m|
    define_method(m) { |*a| File.send(m, *a) }
    module_function m
  end
end

# Dir conveniences on top of the C primitives (entries/children/chdir/…).
class Dir
  def self.home(user = nil)
    return ENV["HOME"] if user.nil? || user == ""
    # /etc/passwd lookup, the portable-enough way.
    File.foreach("/etc/passwd") do |line|
      f = line.split(":")
      return f[5] if f[0] == user
    end rescue nil
    raise ArgumentError, "user #{user} doesn't exist"
  end

  def self.foreach(path, &blk)
    return to_enum(:foreach, path) unless blk
    entries(path).each(&blk)
    nil
  end

  def self.each_child(path, &blk)
    return to_enum(:each_child, path) unless blk
    children(path).each(&blk)
    nil
  end

  def self.empty?(path)
    children(File.path(path)).empty?
  end

  def each_child(&blk)
    return to_enum(:each_child) unless blk
    children.each(&blk)
    self
  end

  # CRuby 3.3+: Dir#chdir — change into this directory.
  def chdir(&blk) = Dir.chdir(path, &blk)

  # dirfd(3) equivalent: an fd opened on the directory, created lazily and
  # owned by this Dir (closed with GC; koruby Dir has no explicit close of it).
  def fileno
    @__dir_fd ||= IO.sysopen(path, File::RDONLY)
  end
end
