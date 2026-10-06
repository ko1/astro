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
    thread.send(:__set_group, self)
    self
  end
  def list; Thread.list.select { |t| t.group.equal?(self) }; end
end
ThreadGroup::Default = ThreadGroup.new

class Thread
  # koruby は runnable が居なくなったら fatal で落ちる (deadlock 検出は常時)。
  # この flag は CRuby 互換の getter/setter だけで、検出の有無は変えられない。
  @__ignore_deadlock = false
  def self.ignore_deadlock; @__ignore_deadlock; end
  def self.ignore_deadlock=(v); @__ignore_deadlock = v; end
  def self.exclusive; yield; end
  def self.exit; current.kill; end        # 明示定義 (無いと explicit-recv quirk で Kernel#exit に落ちる)
  def self.kill(th); th.kill; end
  def group; __group || ThreadGroup::Default; end
  # The running thread's frames are the ones Kernel#caller walks (__caller_strings
  # drops this very frame, so the offsets mean what CRuby's do: 0 = the caller of
  # #backtrace).  Another thread's stack is not reachable from here, so it keeps
  # the stub answer (nil when dead, else a frame naming what it is blocked in).
  def backtrace(*args)
    return __backtrace_other(*args) unless equal?(Thread.current)
    __bt_current('Thread#backtrace', args)
  end
  def backtrace_locations(*args)
    strs = equal?(Thread.current) ? __bt_current('Thread#backtrace_locations', args)
                                  : __backtrace_other(*args)
    strs && strs.map { |s| Thread::Backtrace::Location.new(s) }
  end
  # CRuby reports the call to #backtrace itself as position 0: the same place as
  # the caller's, labelled with the method.  Below it is Kernel#caller's list.
  # __caller_strings(0) drops the #backtrace frame (this `__` helper is not a backtrace frame).
  private def __bt_current(label, args)
    raise ArgumentError, "wrong number of arguments (given #{args.length}, expected 0..2)" if args.length > 2
    list = __caller_strings(0)
    return nil unless list
    list.unshift(list[0].sub(/:in '.*'\z/, ":in '" + label + "'")) unless list.empty?
    return list if args.empty?
    a = args[0]
    if a.is_a?(Range)
      raise ArgumentError, "wrong number of arguments (given #{args.length}, expected 0..2)" if args.length > 1
      return list[a]
    end
    start = a.to_int
    raise ArgumentError, "negative level (#{start})" if start < 0
    return nil if start > list.length
    len = (args.length >= 2 && !args[1].nil?) ? args[1].to_int : nil
    raise ArgumentError, "negative size (#{len})" if len && len < 0
    len ? list[start, len] : list[start..-1]
  end
  # handle_interrupt: マスク列は C 側 (Thread#__int_mask_push) が持ち、配送点が
  # 「今この例外を配ってよいか」を内側のフレームから順に引く。:on_blocking は
  # blocking な配送点 (blop 待ち・join・Mutex) でだけ配られ、Thread.pass のような
  # 非 blocking な点では配られない。
  def self.handle_interrupt(hash, &blk)
    raise ArgumentError, "block is needed" unless blk
    raise ArgumentError, "unknown mask signature" unless hash.is_a?(Hash) && !hash.empty?
    hash.each do |k, v|
      raise TypeError, "class or module required for rescue clause" unless k.is_a?(Module)
      raise ArgumentError, "unknown mask signature" unless [:immediate, :on_blocking, :never].include?(v)
    end
    th = current
    th.send(:__int_mask_push, hash)
    begin
      __check_ints              # :immediate になった pending はブロックの前に走る
      yield
    ensure
      th.send(:__int_mask_pop)
      __check_ints              # マスクが遅らせた分はブロックを出るときに配る
    end
  end
  def self.each_caller_location(*args, **kw, &blk)
    raise ArgumentError, "unknown keyword: #{kw.keys.first.inspect}" unless kw.empty?
    raise LocalJumpError, "no block given" unless blk
    (caller_locations(2) || []).each(&blk)
    nil
  end
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
        raise TypeError, "can't convert #{enum.class} into Array (#{enum.class}#to_a gives #{arr.class})" unless arr.is_a?(Array)
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
        # non_block は close より先: CRuby は閉じた空 queue でも ThreadError
        raise ThreadError, "queue empty" if non_block
        return nil if @__closed
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
  def num_waiting; @__cond.send(:__num_waiting); end
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
  def num_waiting; @__cond.send(:__num_waiting) + @__cond_full.send(:__num_waiting); end
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
        # non_block は close より先: CRuby は閉じた空 queue でも ThreadError
        raise ThreadError, "queue empty" if non_block
        return nil if @__closed
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
[Mutex, Queue, SizedQueue, ConditionVariable].each { __nest_under(_1, Thread) }   # named Thread::Mutex etc.

# Monitor — CRuby では monitor.c の C 実装。stdlib の monitor.rb は
# MonitorMixin だけを足し、Monitor 本体と Monitor::ConditionVariable が
# 既にあることを前提にするので、ここで core として用意する。
class Monitor
  def initialize
    @__mutex = Mutex.new
    @__owner = nil
    @__count = 0
  end

  def enter
    @__mutex.lock unless @__owner == Thread.current
    @__owner = Thread.current
    @__count += 1
    nil
  end
  alias mon_enter enter

  def try_enter
    if @__owner == Thread.current
      @__count += 1
      return true
    end
    return false unless @__mutex.try_lock
    @__owner = Thread.current
    @__count = 1
    true
  end
  alias mon_try_enter try_enter
  alias try_mon_enter try_enter

  def exit
    mon_check_owner
    @__count -= 1
    if @__count == 0
      @__owner = nil
      @__mutex.unlock
    end
    nil
  end
  alias mon_exit exit

  def mon_locked?
    @__mutex.locked?
  end

  def mon_owned?
    @__owner == Thread.current
  end

  def mon_check_owner
    raise ThreadError, "current fiber not owner" unless @__owner == Thread.current
    nil
  end

  # enter / exit inlined: railsbench synchronizes ~10 times a request and each step was a call
  def synchronize
    th = Thread.current
    if @__owner.equal?(th)
      @__count += 1
    else
      @__mutex.lock
      @__owner = th
      @__count = 1
    end
    begin
      yield
    ensure
      raise ThreadError, "current fiber not owner" unless @__owner.equal?(Thread.current)
      if (@__count -= 1) == 0
        @__owner = nil
        @__mutex.unlock
      end
    end
  end
  alias mon_synchronize synchronize

  def new_cond
    ConditionVariable.new(self)
  end

  # ConditionVariable#wait: 再入回数ごと手放して待ち、起きたら同じ深さまで取り直す。
  def wait_for_cond(cond, timeout)
    mon_check_owner
    count = @__count
    @__owner = nil
    @__count = 0
    begin
      cond.__wait(@__mutex, timeout)
    ensure
      @__owner = Thread.current
      @__count = count
    end
  end

  class ConditionVariable
    def initialize(monitor)
      @__monitor = monitor
      @__cond = ::ConditionVariable.new
    end

    def wait(timeout = nil)
      @__monitor.wait_for_cond(self, timeout)
    end

    def wait_until
      until yield
        wait
      end
    end

    def wait_while
      while yield
        wait
      end
    end

    def signal
      @__monitor.mon_check_owner
      @__cond.signal
      self
    end

    def broadcast
      @__monitor.mon_check_owner
      @__cond.broadcast
      self
    end

    def __wait(mutex, timeout)   # :nodoc: Monitor#wait_for_cond からのみ
      @__cond.wait(mutex, timeout)
    end
  end
end

Thread.const_set(:Monitor, Monitor)
__nest_under(Monitor, Thread)

module Process
  # clock_gettime(2) の clockid_t (Linux)。koruby は実際には _clk を無視して
  # monotonic を返すが、定数は本物の値で揃えておく。
  CLOCK_REALTIME           = 0
  CLOCK_MONOTONIC          = 1
  CLOCK_PROCESS_CPUTIME_ID = 2
  CLOCK_THREAD_CPUTIME_ID  = 3
  CLOCK_MONOTONIC_RAW      = 4
  CLOCK_REALTIME_COARSE    = 5
  CLOCK_MONOTONIC_COARSE   = 6
  CLOCK_BOOTTIME           = 7
  CLOCK_REALTIME_ALARM     = 8
  CLOCK_BOOTTIME_ALARM     = 9
  CLOCK_TAI                = 11
  def self.pid; __getpid; end              # not $$: that is captured at boot and stale after fork
  class << self
    # CRuby と同じく fork(2) 本体は Process._fork にあり、fork はそれを呼ぶ
    # (差し替えれば fork の側にも効く)。
    def _fork = __fork_raw
    def fork(&blk)
      pid = _fork
      return pid unless pid == 0
      return nil unless blk                # 子・ブロック無し: nil
      begin                                # 子: ブロックを走らせて終了する
        blk.call
        __fork_finish(0)
      rescue Exception => e
        __fork_finish(e)
      end
    end
  end
  # ids are re-read every call: fork (and set*id) change them
  def self.uid  = __process_ids[0]
  def self.euid = __process_ids[1]
  def self.gid  = __process_ids[2]
  def self.egid = __process_ids[3]
  def self.ppid = __process_ids[4]
  def self.clock_gettime(clk = CLOCK_MONOTONIC, unit = :float_second)
    t = __clock_gettime(clk.is_a?(Integer) ? clk : CLOCK_MONOTONIC)
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
    raise TypeError, "can't convert #{v.class} into Integer" unless n.is_a?(Integer)
    n
  end
  private_class_method :__int_arg
  # Process::Tms — CRuby と同じ Struct 型。値は getrusage(2) から取る
  # (0 固定だと "1 until Process.times.utime > user" が終わらない)。
  Tms = Struct.new(:utime, :stime, :cutime, :cstime)
  def self.times
    u, s, cu, cs = __process_times
    Tms.new(u, s, cu, cs)
  end

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
    # CRuby asks anything else for #to_str first, and only then for #to_int.
    v = String.try_convert(v) || v unless v.is_a?(Symbol) || v.is_a?(String)
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
    raise TypeError, "can't convert #{v.class} into Integer (#{v.class}#to_int gives #{i.class})" unless i.is_a?(Integer)
    i
  end
  private_class_method :__as_rlimit_int

  # 実クロックは 1ns。CRuby が別実装で埋めている擬似クロックだけ固有の分解能を返す。
  PSEUDO_CLOCK_RES__ = {
    GETTIMEOFDAY_BASED_CLOCK_REALTIME: 1_000,
    TIME_BASED_CLOCK_REALTIME: 1_000_000_000,
    GETRUSAGE_BASED_CLOCK_PROCESS_CPUTIME_ID: 1_000,
    TIMES_BASED_CLOCK_PROCESS_CPUTIME_ID: 10_000_000,
    TIMES_BASED_CLOCK_MONOTONIC: 10_000_000,
    CLOCK_BASED_CLOCK_PROCESS_CPUTIME_ID: 1_000,
  }.freeze
  private_constant :PSEUDO_CLOCK_RES__

  def self.clock_getres(_clk = CLOCK_MONOTONIC, unit = :float_second)
    ns = PSEUDO_CLOCK_RES__[_clk] || 1
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
  # CRuby: the reaper is a Process::Waiter (a Thread) whose #pid reads the
  # thread-local :pid; a vanished child is tolerated (the thread just ends nil).
  class Waiter < Thread
    def pid = self[:pid]
  end
  def self.detach(pid)
    unless pid.is_a?(Integer)                        # NUM2PIDT: #to_int, and it must give an Integer
      raise TypeError, "no implicit conversion of #{pid.class} into Integer" unless pid.respond_to?(:to_int)
      n = pid.to_int
      raise TypeError, "can't convert #{pid.class} into Integer (#{pid.class}#to_int gives #{n.class})" unless n.is_a?(Integer)
      pid = n
    end
    w = Waiter.new(pid) do |p|
      begin
        Process.wait2(p)[1]
      rescue Errno::ECHILD
        nil
      end
    end
    w[:pid] = pid                  # before the caller sees it (CRuby sets it at creation)
    w
  end

  # id= accepts an Integer or a user/group *name* (CRuby resolves via getpwnam(3)).
  def self.__uid_arg(v)
    return v if v.is_a?(Integer)
    if v.is_a?(String)
      require 'etc'
      pw = (Etc.getpwnam(v) rescue nil)
      raise ArgumentError, "can't find user for #{v}" unless pw
      return pw.uid
    end
    raise TypeError, "no implicit conversion of #{v.class} into Integer"
  end
  def self.__gid_arg(v)
    return v if v.is_a?(Integer)
    if v.is_a?(String)
      require 'etc'
      gr = (Etc.getgrnam(v) rescue nil)
      raise ArgumentError, "can't find group for #{v}" unless gr
      return gr.gid
    end
    raise TypeError, "no implicit conversion of #{v.class} into Integer"
  end
  def self.uid=(v);  __set_id(0, __uid_arg(v)); end
  def self.euid=(v); __set_id(1, __uid_arg(v)); end
  def self.gid=(v);  __set_id(2, __gid_arg(v)); end
  def self.egid=(v); __set_id(3, __gid_arg(v)); end

  module Sys
    def self.getuid = Process.uid
    def self.geteuid = Process.euid
    def self.getgid = Process.gid
    def self.getegid = Process.egid
    def self.setuid(v)  = __set_id(0, v)
    def self.seteuid(v) = __set_id(1, v)
    def self.setruid(v) = __set_reid(0, v, -1)
    def self.setgid(v)  = __set_id(2, v)
    def self.setegid(v) = __set_id(3, v)
    def self.setrgid(v) = __set_reid(1, v, -1)
    def self.setreuid(r, e) = __set_reid(0, r, e)
    def self.setregid(r, e) = __set_reid(1, r, e)
    def self.issetugid = false
  end

  module UID
    def self.rid = Process.uid
    def self.eid = Process.euid
    def self.eid=(v); Process.euid = v; end
    def self.change_privilege(v) = (Process.uid = v; Process.uid)
    def self.grant_privilege(v) = (Process.euid = v; Process.euid)
    def self.re_exchange
      r, e = Process.uid, Process.euid
      __set_reid(0, e, r)
      Process.euid
    end
    def self.re_exchangeable? = true
    def self.sid_available? = true
    def self.switch
      r, e = Process.uid, Process.euid
      __set_reid(0, e, r)
      return Process.euid unless block_given?
      begin
        yield
      ensure
        __set_reid(0, r, e)
      end
    end
    def self.from_name(name) = Process.__uid_arg(name)
  end

  module GID
    def self.rid = Process.gid
    def self.eid = Process.egid
    def self.eid=(v); Process.egid = v; end
    def self.change_privilege(v) = (Process.gid = v; Process.gid)
    def self.grant_privilege(v) = (Process.egid = v; Process.egid)
    def self.re_exchange
      r, e = Process.gid, Process.egid
      __set_reid(1, e, r)
      Process.egid
    end
    def self.re_exchangeable? = true
    def self.sid_available? = true
    def self.switch
      r, e = Process.gid, Process.egid
      __set_reid(1, e, r)
      return Process.egid unless block_given?
      begin
        yield
      ensure
        __set_reid(1, r, e)
      end
    end
    def self.from_name(name) = Process.__gid_arg(name)
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
  def self.start(*); __gc_start; nil; end
  def self.enable; prev = @__disabled; @__disabled = false; __gc_disable(false); !!prev; end
  def self.disable; prev = @__disabled; @__disabled = true; __gc_disable(true); !!prev; end
  # GC#garbage_collect (module instance method; include GC で使う)
  def garbage_collect(*); GC.start; nil; end
  # stat: 実 GC の統計を CRuby 形のキーで返す (koruby GC にある分だけ実数、
  # 残りは 0)。stat(hash) は既存 hash を更新して返し、stat(:key) は単値。
  def self.stat(arg = nil)
    cnt, minor, major, bytes, ns = __gc_stat_raw
    h = {
      count: cnt, minor_gc_count: minor, major_gc_count: major,
      total_allocated_objects: 0, total_freed_objects: 0,
      heap_allocated_pages: 0, heap_sorted_length: 0, heap_allocatable_pages: 0,
      heap_available_slots: 0, heap_live_slots: 0, heap_free_slots: 0,
      heap_final_slots: 0, heap_marked_slots: 0, heap_eden_pages: 0,
      heap_tomb_pages: 0, total_allocated_pages: 0, total_freed_pages: 0,
      malloc_increase_bytes: bytes, malloc_increase_bytes_limit: 0,
      compact_count: 0,
      read_barrier_faults: 0, total_moved_objects: 0,
      remembered_wb_unprotected_objects: 0, remembered_wb_unprotected_objects_limit: 0,
      old_objects: 0, old_objects_limit: 0, oldmalloc_increase_bytes: 0,
      oldmalloc_increase_bytes_limit: 0, marking_time: 0, sweeping_time: 0,
      time: ns / 1_000_000,
    }
    case arg
    when nil then h
    when Symbol
      raise ArgumentError, "unknown key: #{arg}" unless h.key?(arg)
      h[arg]
    when Hash
      arg.each_key { |k| arg[k] = h[k] if h.key?(k) }
      arg
    else
      raise TypeError, "non-hash or symbol given"
    end
  end
  def self.count; __gc_stat_raw[0]; end
  # Wired to the collector's real stress flag (the one BARUBY_GC_STRESS sets),
  # so `GC.stress = true` genuinely collects at every allocation.  The assigned
  # value is remembered verbatim because CRuby's reader hands back what was set
  # (it also accepts an Integer bitmask), not a normalized boolean.
  def self.stress; @__stress.nil? ? __gc_stress : @__stress; end
  def self.stress=(v); __gc_stress(v); @__stress = v; end
  # 計測系は koruby GC (precise copying) では未提供。CRuby と同じ「形」だけ
  # 返して、参照するだけのコードが NoMethodError にならないようにする。
  CONFIG_KEYS__ = [:rgengc_allow_full_mark].freeze         # 書き込み可能な既知キー
  def self.config(hash = nil)
    @__gc_config ||= { implementation: "koruby-precise" }
    if hash
      raise ArgumentError, "expected keyword arguments" unless hash.is_a?(Hash)
      hash.each do |k, v|
        ks = k.to_sym
        raise ArgumentError, "Attempting to set read-only key \"Implementation\"" if ks == :implementation
        @__gc_config[ks] = v if CONFIG_KEYS__.include?(ks)  # 未知キーは無視 (CRuby)
      end
    end
    @__gc_config.dup
  end

  # GC::Profiler: 形だけ (koruby GC は per-GC プロファイルを取らない)。
  module Profiler
    @enabled = false
    def self.enabled?; @enabled; end
    def self.enable; @enabled = true; nil; end
    def self.disable; @enabled = false; nil; end
    def self.clear; nil; end
    def self.report(*); nil; end
    def self.result; ""; end
    def self.raw_data; @enabled ? [] : nil; end
    def self.total_time; 0.0; end
  end
  def self.total_time; 0; end
  def self.measure_total_time; @__gc_measure.nil? ? true : @__gc_measure; end
  def self.measure_total_time=(v); @__gc_measure = v ? true : false; v; end
  def self.auto_compact; !!@__auto_compact; end
  def self.auto_compact=(v); @__auto_compact = v; v; end
  # the collector moves objects on every collection, so compacting is a full GC; per-type counts are not kept
  def self.compact; __gc_start; latest_compact_info; end
  def self.latest_compact_info; { considered: {}, moved: {}, moved_up: {}, moved_down: {} }; end
  def self.verify_compaction_references(toward: nil, double_heap: false, expand_heap: false); compact; end
  def self.latest_gc_info(arg = nil); arg.is_a?(Symbol) ? nil : {}; end
end

module ObjectSpace
  # The copying GC's arena is a gapless bump region, so __heap_objects__ can walk
  # it (see docs/objectspace_heap_walk.md).  It returns a snapshot Array rather
  # than yielding, because a block that allocates would move the heap under the
  # walk cursor.  As in CRuby the walk can also surface not-yet-collected
  # garbage; nothing is yielded twice.
  def self.each_object(*args, &blk)
    if args.size > 1
      raise ArgumentError, "wrong number of arguments (given #{args.size}, expected 0..1)"
    end
    mod = args[0]
    raise TypeError, "class or module required" if args.size == 1 && !mod.is_a?(Module)
    return to_enum(:each_object, *args) if blk.nil?
    objs = __heap_objects__(mod)
    objs.each { |o| blk.call(o) }
    objs.size
  end
  def self.count_objects(*); {}; end
  def self.garbage_collect(*); nil; end

  # Deprecated in Ruby 4.0 and warned about on every call.  The immediate ids
  # are the exact inverse of Object#object_id; a heap id is an address, so it is
  # resolved by the same heap walk each_object uses.  A moving GC can hand out
  # the same address twice over a program's life, which is precisely why CRuby
  # deprecated this.
  def self._id2ref(id)
    warn "warning: ObjectSpace._id2ref is deprecated" unless $VERBOSE.nil?
    raise TypeError, "not an id value" unless id.is_a?(Integer)
    return false if id == 0
    return nil   if id == 4
    return true  if id == 20
    return (id - 1) / 2 if id.odd?
    found = __heap_objects__(nil).find { |o| o.object_id == id }
    raise RangeError, "#{id} is not an id value" if found.nil?
    found
  end

  # Finalizers.  koruby's GC has no per-object finalization hook, so a finalizer
  # never fires on collection — but it does fire at process exit, which is the
  # only timing CRuby actually guarantees.  The registry holds the object itself
  # (both to keep it alive and because object_id is not stable across a moving
  # GC), so entries are matched by identity, not by id.
  FINALIZERS__ = []                      # [[obj, [callable, ...]], ...]

  def self.define_finalizer(obj, callable = nil, &blk)
    callable = blk if callable.nil?
    raise ArgumentError, "no finalizer given" if callable.nil?
    unless callable.respond_to?(:call)
      raise ArgumentError, "wrong type argument #{callable.class} (should be callable)"
    end
    # only a real (heap) object can carry one; immediates cannot be finalized
    case obj
    when Integer, Symbol, Float, NilClass, TrueClass, FalseClass
      raise ArgumentError, "cannot define finalizer for #{obj.class}"
    end
    raise FrozenError, "can't modify frozen #{obj.class}: #{obj.inspect}" if obj.frozen?
    # A finalizer whose receiver is the object keeps it alive forever, so CRuby
    # warns about it.  The receiver is the Proc's self / the Method's receiver.
    __warn_self_referential_finalizer__(obj, callable)
    obj.__id__                     # an object with a finalizer always has an id: #dup / #clone ask only those
    entry = FINALIZERS__.find { |e| e[0].equal?(obj) }
    if entry.nil?
      entry = [obj, []]
      FINALIZERS__ << entry
    end
    # CRuby registers a given callable once per object and hands back the one it
    # already holds, so re-registering an equal callable is a no-op.
    existing = entry[1].find { |f| f.equal?(callable) || f == callable }
    if existing
      [0, existing]
    else
      entry[1] << callable
      __install_finalizer_hook__
      __finalizers_active__        # lets #clone know it has to copy entries
      [0, callable]
    end
  end

  def self.__warn_self_referential_finalizer__(obj, callable)
    return if $VERBOSE.nil?               # -W0 silences it, as rb_warn does
    recv = case callable
           when Proc   then (callable.binding.receiver rescue nil)
           when Method then callable.receiver
           end
    warn "warning: finalizer references object to be finalized" if recv.equal?(obj)
  end
  private_class_method :__warn_self_referential_finalizer__

  # #dup / #clone copy the original's finalizers onto the copy (CRuby does this
  # in init_copy, which both share).  Called from the C copy path, so it skips
  # define_finalizer's frozen and immediate checks -- the copy is what it is.
  def self.__copy_finalizers__(orig, copy)
    entry = FINALIZERS__.find { |e| e[0].equal?(orig) }
    return copy if entry.nil? || entry[1].empty?
    copy.__id__
    FINALIZERS__ << [copy, entry[1].dup]
    copy
  end

  def self.undefine_finalizer(obj)
    raise FrozenError, "can't modify frozen #{obj.class}: #{obj.inspect}" if obj.frozen?
    FINALIZERS__.reject! { |e| e[0].equal?(obj) }
    obj
  end

  # Run every registered finalizer with the object's id.  A finalizer may itself
  # register more (CRuby runs those too), so the drain loops until the registry
  # is empty.  An exception from one is reported and the rest still run.
  def self.__run_finalizers__
    until FINALIZERS__.empty?
      obj, list = FINALIZERS__.shift
      id = obj.object_id
      list.reverse_each do |f|
        begin
          f.call(id)
        rescue Exception => e
          # rb_warn's wording and gating: silent only when $VERBOSE is nil (-W0).
          unless $VERBOSE.nil?
            warn "warning: Exception in finalizer #{f.inspect}"
            warn "#{e.class}: #{e.message}"
          end
        end
      end
    end
  end

  def self.__install_finalizer_hook__
    return if @finalizer_hook
    @finalizer_hook = true
    at_exit { ObjectSpace.__run_finalizers__ }
  end
end

# 定義済みグローバル (CRuby 初期値)
$/ = "\n".encode(Encoding::US_ASCII).freeze   # input record separator (CRuby: frozen US-ASCII)
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
  include Enumerable            # CRuby: IO (and so File) is Enumerable over #each_line
  # a fresh, mutable String every call (CRuby builds one from the fptr's path)
  def to_path
    s = @__io_path
    return +"<#{@__io_std_name}>" if s.nil? && @__io_std_name   # CRuby names the std streams this way
    s.is_a?(String) ? (+s.dup) : s
  end
  alias path to_path            # CRuby: #path IS #to_path
  # io/wait: IO#wait.  CRuby's io_wait has two shapes: exactly (events, timeout)
  # with no Symbol answers the ready-event mask; anything else is the legacy
  # form (Symbols + at most one timeout) and answers self / nil.
  def wait(*args)
    if args.length == 2 && !args[0].is_a?(Symbol) && !args[1].is_a?(Symbol)
      ev = args[0]
      raise TypeError, "no implicit conversion of nil into Integer" if ev.nil?
      ev = ev.to_int if !ev.is_a?(Integer) && ev.respond_to?(:to_int)
      raise TypeError, "no implicit conversion of #{ev.class} into Integer" unless ev.is_a?(Integer)
      raise ArgumentError, "Events must be positive integer!" if ev <= 0
      r = __wait_events(__io_poll(ev, args[1]), ev)
      return r.zero? ? nil : r
    end
    events = 0
    timeout = nil
    seen = false
    args.each do |x|
      if x.is_a?(Symbol)
        events |= case x
                  when :read, :r, :readable then READABLE
                  when :write, :w, :writable then WRITABLE
                  when :read_write, :rw, :readable_writable then READABLE | WRITABLE
                  else raise ArgumentError, "unsupported mode: #{x}"
                  end
      else
        raise ArgumentError, "timeout given more than once" if seen
        raise TypeError, "can't convert #{x.nil? || x == true || x == false ? x.inspect : x.class} into time interval" unless x.is_a?(Numeric)
        raise ArgumentError, "time interval must not be negative" if x < 0
        timeout = x
        seen = true
      end
    end
    events = READABLE if events.zero?
    __wait_events(__io_poll(events, timeout), events).zero? ? nil : self
  end
  # io/wait: priority (POLLPRI) data ready → self, else nil after the timeout
  def wait_priority(timeout = nil)
    __wait_events(__io_poll(PRIORITY, timeout), PRIORITY).zero? ? nil : self
  end
  # poll(2) revents → IO::READABLE/WRITABLE/PRIORITY the way CRuby maps them:
  # HUP counts as readable, and an error readies every requested event.
  private def __wait_events(revents, events)
    r = 0
    r |= READABLE if (revents & 0x19) != 0      # POLLIN | POLLERR | POLLHUP
    r |= WRITABLE if (revents & 0x0c) != 0      # POLLOUT | POLLERR
    r |= PRIORITY if (revents & 0x02) != 0      # POLLPRI
    r |= events if (revents & 0x28) != 0        # POLLERR | POLLNVAL
    r
  end
end

class File
  # File-only in CRuby (an IO / pipe has no #size or file times)
  def size          # CRuby flushes the write buffer first
    flush
    stat.size
  end
  # The open file's times.  koruby re-stats the path it was opened with, which
  # is all the fd-less File::Stat representation can offer.
  def atime = File.atime(__time_path)
  def ctime = File.ctime(__time_path)
  def mtime = File.mtime(__time_path)
  def birthtime = File.birthtime(__time_path)
  private def __time_path
    raise IOError, "closed stream" if closed?
    @__io_path or raise NotImplementedError, "file times are unavailable for a descriptor with no path"
  end
  # File#lstat — stat の symlink 版。開いているのはファイル本体なので、
  # 開いたときのパスを lstat し直す (パスがなければ #stat と同じ)。
  def lstat
    @__io_path ? File.lstat(@__io_path) : stat
  end
  # NULL / LOCK_* / SEPARATOR* are defined in C (File::Constants / File).
  # LOCK_NB answers false when the lock is held elsewhere; without it, wait.
  def flock(op)
    op = op.to_int unless op.is_a?(Integer)
    loop do
      r = __flock_nb(op)
      return r if r == 0 || (op & File::LOCK_NB) != 0
      sleep 0.01
    end
  end

  # File.path(obj) — the path String an object names: #to_path if it has one,
  # else the String itself (#to_str-coerced).  Pathname is built on this.
  def self.path(obj)
    s = if obj.is_a?(String) then obj
        elsif obj.respond_to?(:to_path)
          r = obj.to_path
          r.is_a?(String) ? r : (r.respond_to?(:to_str) ? r.to_str : r)
        elsif obj.respond_to?(:to_str) then obj.to_str
        else obj
        end
    unless s.is_a?(String)
      raise TypeError, "no implicit conversion of #{obj.nil? ? 'nil' : obj.class} into String"
    end
    unless s.encoding.ascii_compatible?   # before the NUL scan: UTF-16/32 paths are full of NUL bytes
      raise Encoding::CompatibilityError, "path name must be ASCII-compatible (#{s.encoding}): #{s.inspect}"
    end
    raise ArgumentError, "path name contains null byte" if s.include?("\0")
    s
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
      @current = $stdin
      @current_name = "-"
    else
      @current_name = @argv.shift.to_s
      @current = @current_name == "-" ? $stdin : File.open(@current_name, @binmode ? "rb" : "r")
    end
    @opened_any = true
    @advance = false
    @file_lineno = 0
    __set_gvar("$FILENAME", @current_name)   # read-only for user code
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
  # `probe`: skip a file already at EOF by asking #eof? — which parks on an
  # empty pipe, so the non-blocking readers pass false and let the read decide.
  def __stream(probe = true)
    loop do
      if @current.nil? || @advance
        return nil unless __next_file
      end
      return @current unless @current.closed? || (probe && @current.eof?)
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
      res = nil                              # the first chunk sets the encoding
      while (io = __stream)
        s = io.read.to_s
        res = res ? res << s : s
        __finish_current
      end
      res ||= "".dup.force_encoding(@binmode ? Encoding::BINARY : Encoding.default_external)
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

  # readpartial / read_nonblock do not silently cross a file boundary.  Reaching
  # the end of one file advances to the next but yields ONE empty read for that
  # call, so the caller sees the boundary; EOFError comes only once every file
  # is exhausted.
  def __partial(meth, maxlen, buffer, **kw)
    had = @current
    io = __stream(false)
    raise EOFError, "end of file reached" if io.nil?
    # __stream silently skips a file that is already at EOF; that seam is what
    # the caller must see as one empty read before the next file's bytes.
    return(buffer ? buffer.replace("") : "") if had && !had.equal?(io)
    begin
      buffer ? io.send(meth, maxlen, buffer, **kw) : io.send(meth, maxlen, **kw)
    rescue EOFError
      __finish_current
      raise EOFError, "end of file reached" if __stream(false).nil?
      buffer ? buffer.replace("") : ""
    end
  end
  private :__partial

  def readpartial(maxlen, buffer = nil); __partial(:readpartial, maxlen, buffer); end

  def read_nonblock(maxlen, buffer = nil, exception: true)
    # CRuby checks the keyword before it looks at any stream, so an exhausted
    # ARGF still reports the bad argument rather than EOFError.
    unless exception == true || exception == false
      raise ArgumentError, "expected true or false as exception: #{exception.inspect}"
    end
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
ARGF.instance_variable_set(:@argv, ARGV)    # ARGF.argv IS ARGV (CRuby)
__set_gvar("$<", ARGF)   # the default input stream (read-only for user code)
__set_gvar("$*", ARGV)   # $* is ARGV
$> = $stdout       # the default output stream ($DEFAULT_OUTPUT)
$. = 0             # input line number, bumped by every #gets

module ObjectSpace
  # WeakMap: rows in the VM's weak table (__wmap_op), dropped by the GC once the
  # key, the value or the map itself is collected.  Keys compare by identity.
  # WeakKeyMap below still holds its entries strongly.
  class WeakMap
    include Enumerable

    def [](k) = __wmap_op(self, 0, k)
    def []=(k, v)
      __wmap_op(self, 1, k, v)
    end
    def include?(k) = __wmap_op(self, 3, k)
    alias_method :key?, :include?
    alias_method :member?, :include?
    def delete(k, &blk)
      return __wmap_op(self, 2, k) if __wmap_op(self, 3, k)
      blk ? blk.call(k) : nil
    end
    def size = __wmap_op(self, 5)
    alias_method :length, :size
    private def __pairs = __wmap_op(self, 4).each_slice(2).to_a
    def keys = __pairs.map(&:first)
    def values = __pairs.map(&:last)

    # `each` on an empty map returns self even without a block; with entries and
    # no block it is a LocalJumpError (CRuby yields eagerly, it is not an
    # Enumerator-returning method).
    def each(&blk)
      ps = __pairs
      return self if ps.empty? && blk.nil?
      raise LocalJumpError, "no block given (yield)" if blk.nil?
      ps.each { |k, v| blk.call(k, v) }
      self
    end
    alias_method :each_pair, :each

    def each_key(&blk)
      ps = __pairs
      return self if ps.empty? && blk.nil?
      raise LocalJumpError, "no block given (yield)" if blk.nil?
      ps.each { |k, _| blk.call(k) }
      self
    end

    def each_value(&blk)
      ps = __pairs
      return self if ps.empty? && blk.nil?
      raise LocalJumpError, "no block given (yield)" if blk.nil?
      ps.each { |_, v| blk.call(v) }
      self
    end

    def inspect
      body = __pairs.map { |k, v| "#{ObjectSpace.__any_to_s(k)} => #{ObjectSpace.__any_to_s(v)}" }.join(", ")
      format("#<ObjectSpace::WeakMap:0x%016x%s>", object_id, body.empty? ? "" : ": #{body}")
    end
  end

  # `#<Class:0xaddr>` for any object, including a BasicObject (which has no
  # #inspect / #class to call).
  def self.__any_to_s(o)
    case o                                            # special constants inspect (CRuby's wmap_inspect)
    when Integer, Symbol, Float, nil, true, false then return o.inspect
    end
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
    # in C (__gc_collectable?) so nothing is dispatched to the key itself (a
    # BasicObject key must fail on #hash, not on #nil?).
    def __collectable?(key) = __gc_collectable?(key)
  end
end

# `Module.constants` (no arguments) is a special form: it answers the constants
# visible at the caller's scope, which at top level is Object's.  With an
# argument it is the ordinary Module#constants.  Only for Module itself: Class
# and every class's singleton class inherit this method (rb_mod_s_constants).
class << Module
  alias_method :__constants_of, :constants
  private :__constants_of
  def constants(*args) = (args.empty? && equal?(Module)) ? Object.constants : __constants_of(*args)
end

module RbConfig
  CONFIG = {
    "host_os" => "linux", "host_cpu" => "x86_64", "arch" => "x86_64-linux",
    "ruby_install_name" => "koruby", "RUBY_INSTALL_NAME" => "koruby",
    "EXEEXT" => "", "bindir" => "/usr/bin", "rubylibdir" => "/usr/lib/ruby",
    "ruby_version" => RUBY_VERSION, "MAJOR" => RUBY_VERSION.split(".")[0],
    "MINOR" => RUBY_VERSION.split(".")[1], "TEENY" => RUBY_VERSION.split(".")[2],
  }
  CONFIG["PATCHLEVEL"] = RUBY_PATCHLEVEL.to_s
  CONFIG["UNICODE_VERSION"] = "17.0.0"      # the tables in builtins/unicode_case.h
  CONFIG["UNICODE_EMOJI_VERSION"] = "17.0"
  CONFIG["DLEXT"] = "kompat.so"          # CRuby extensions rebuilt for koruby's compatibility layer (builtins/cext.c)
  CONFIG["target_os"] = CONFIG["host_os"]
  CONFIG["target_cpu"] = CONFIG["host_cpu"]
  CONFIG["CROSS_COMPILING"] = "no"
  CONFIG["ENABLE_SHARED"] = "no"
  CONFIG["LIBRUBY"] = "libkoruby.a"
  CONFIG["LIBRUBY_SO"] = "libkoruby.so"
  CONFIG["LIBPATHENV"] = "LD_LIBRARY_PATH"
  CONFIG["libdirname"] = "libdir"
  CONFIG["libdir"] = "/usr/lib"
  CONFIG["AR"] = "ar"
  CONFIG["STRIP"] = "strip -S -x"
  CONFIG["archdir"] = CONFIG["rubylibdir"]

  # --- paths -----------------------------------------------------------------
  # koruby runs out of its build directory rather than an installed prefix, so
  # the whole layout is derived from ONE root.  rubygems (Gem.default_dir and
  # friends) and mkmf read these, and they must be mutually consistent or
  # rubygems dies on a nil in File.join.  KORUBY_PREFIX overrides the root.
  # The default root is root/ beside the binary (tools/make_root.sh: stdlib, rubygems,
  # the bundled gems), so koruby alone requires what CRuby would.
  __root = (File.join(File.dirname(File.readlink("/proc/self/exe")), "root") rescue nil)
  __root = nil unless __root && File.directory?(__root)
  __pfx = ENV["KORUBY_PREFIX"] || __root || "/usr"
  # CRuby's "ruby_version" is the ABI version, which for a dev build carries a
  # "+N" suffix (4.1.0+4).  rubygems joins it into Gem.default_dir, so pointing
  # KORUBY_PREFIX at such an install must use the directory that is really
  # there or every default gem goes missing.
  __rv  = RUBY_VERSION
  unless File.directory?("#{__pfx}/lib/ruby/#{__rv}")
    __abi = Dir.glob("#{__pfx}/lib/ruby/#{__rv}+*").sort.last
    __rv = File.basename(__abi) if __abi
  end
  CONFIG["ruby_version"] = __rv
  __arch = CONFIG["arch"]
  CONFIG["prefix"] = __pfx
  CONFIG["exec_prefix"] = __pfx
  CONFIG["bindir"] = "#{__pfx}/bin"
  # bindir / ruby_install_name は「いま走っているインタプリタ」を指す。
  # KORUBY_PREFIX は stdlib の置き場の指定であって binary の置き場ではないので、
  # prefix から組み立てると実在しないパスになる。rubygems は
  # Gem.ruby = File.join(bindir, ruby_install_name + EXEEXT) で子プロセスを起動する
  # (rake のサブプロセステストはこれが実在しないと全滅する)。
  # The running binary itself, not an env var: setting RUBY_EXE at startup leaked
  # it into every child's environment (rspec-core's with_clean_environment).
  # mspec finds us through bindir/ruby_install_name the same way.
  __exe = (File.readlink("/proc/self/exe") rescue nil) || ENV["RUBY_EXE"]
  if __exe && !__exe.empty? && File.exist?(__exe)
    CONFIG["bindir"] = File.dirname(__exe)
    CONFIG["ruby_install_name"] = File.basename(__exe)
    CONFIG["RUBY_INSTALL_NAME"] = CONFIG["ruby_install_name"]
  end
  CONFIG["libdir"] = "#{__pfx}/lib"
  CONFIG["libexecdir"] = "#{__pfx}/libexec"
  CONFIG["datarootdir"] = "#{__pfx}/share"
  CONFIG["datadir"] = "#{__pfx}/share"
  CONFIG["sysconfdir"] = "#{__pfx}/etc"
  CONFIG["localstatedir"] = "/var"
  CONFIG["includedir"] = "#{__pfx}/include"
  CONFIG["rubylibprefix"] = "#{__pfx}/lib/ruby"
  CONFIG["rubylibdir"] = "#{__pfx}/lib/ruby/#{__rv}"
  CONFIG["rubyarchprefix"] = "#{__pfx}/lib/ruby/#{__arch}"
  CONFIG["rubyarchdir"] = "#{__pfx}/lib/ruby/#{__rv}/#{__arch}"
  CONFIG["archdir"] = CONFIG["rubyarchdir"]
  CONFIG["sitedir"] = "#{__pfx}/lib/ruby/site_ruby"
  CONFIG["sitelibdir"] = "#{__pfx}/lib/ruby/site_ruby/#{__rv}"
  CONFIG["sitearchdir"] = "#{__pfx}/lib/ruby/site_ruby/#{__rv}/#{__arch}"
  CONFIG["vendordir"] = "#{__pfx}/lib/ruby/vendor_ruby"
  CONFIG["vendorlibdir"] = "#{__pfx}/lib/ruby/vendor_ruby/#{__rv}"
  CONFIG["vendorarchdir"] = "#{__pfx}/lib/ruby/vendor_ruby/#{__rv}/#{__arch}"
  # CRuby's default $LOAD_PATH tail (site → vendor → rubylibdir; koruby's own
  # lib stands in for the last).  rubygems inserts an activated gem's lib at the
  # index of sitelibdir and falls back to 0 — IN FRONT of -I — when that entry
  # is missing, which shadowed a checked-out gem with its installed copy.
  # copies, as in CRuby: did_you_mean deep-freezes the load path it snapshots
  # (Ractor.make_shareable), which must not freeze CONFIG's own values
  $LOAD_PATH.unshift(CONFIG["sitelibdir"].dup, CONFIG["sitearchdir"].dup,
                     CONFIG["vendorlibdir"].dup, CONFIG["vendorarchdir"].dup)
  # root's CRuby stdlib goes last, behind koruby's own lib/ (which replaces its C-backed parts)
  $LOAD_PATH.push(CONFIG["rubylibdir"].dup) if __root && __pfx == __root
  CONFIG["rubyhdrdir"] = "#{__pfx}/include/ruby-#{__rv}"
  CONFIG["rubyarchhdrdir"] = "#{__pfx}/include/ruby-#{__rv}/#{__arch}"
  CONFIG["sitehdrdir"] = "#{__pfx}/include/ruby-#{__rv}/site_ruby"
  CONFIG["vendorhdrdir"] = "#{__pfx}/include/ruby-#{__rv}/vendor_ruby"
  CONFIG["mandir"] = "#{__pfx}/share/man"
  CONFIG["docdir"] = "#{__pfx}/share/doc/$(PACKAGE)"
  CONFIG["RUBY_BASE_NAME"] = "ruby"
  CONFIG["RUBY_PROGRAM_VERSION"] = __rv
  CONFIG["RUBY_API_VERSION"] = __rv.split(".")[0, 2].join(".")
  CONFIG["RUBY_SO_NAME"] = "koruby"
  CONFIG["LIBRUBY_A"] = "libkoruby.a"
  CONFIG["LIBRUBYARG"] = "-lkoruby"
  CONFIG["LIBRUBYARG_SHARED"] = "-lkoruby"
  CONFIG["LIBRUBYARG_STATIC"] = "-lkoruby-static"
  CONFIG["LIBEXT"] = "a"
  CONFIG["OBJEXT"] = "o"
  CONFIG["EXECUTABLE_EXTS"] = ""            # "" on unix; optparse splits it
  CONFIG["NULLCMD"] = ":"
  CONFIG["PATH_SEPARATOR"] = ":"
  CONFIG["SOEXT"] = "so"
  CONFIG["host"] = __arch
  CONFIG["host_vendor"] = "pc"
  CONFIG["target"] = __arch
  CONFIG["target_vendor"] = "pc"
  CONFIG["build"] = __arch
  CONFIG["build_os"] = CONFIG["host_os"]
  CONFIG["build_cpu"] = CONFIG["host_cpu"]
  CONFIG["build_vendor"] = "pc"
  # CRuby leaves the values unfrozen even under --enable-frozen-string-literal.
  CONFIG.transform_values! { |v| v.frozen? ? +v : v }
  # mkmf's MAKEFILE_CONFIG (and the build keys of CONFIG: CC, CFLAGS, LDSHARED ...),
  # written by tools/make_root.sh; loaded only when something names it
  __mk = "#{CONFIG["rubylibdir"]}/#{__arch}/koruby_makefile_config.rb"
  autoload :MAKEFILE_CONFIG, __mk if File.exist?(__mk)

  # nil unless koruby is installed under a prefix (the spec guards on it).
  TOPDIR = nil

  # The C types this build uses; the spec only requires String keys and
  # Integer values plus the documented handful.
  # generated from CRuby 4.1.0dev x86_64-linux (rbconfig/sizeof)
  SIZEOF = {
    "int" => 4,
    "short" => 2,
    "long" => 8,
    "long long" => 8,
    "__int128" => 16,
    "off_t" => 8,
    "void*" => 8,
    "float" => 4,
    "double" => 8,
    "time_t" => 8,
    "clock_t" => 8,
    "size_t" => 8,
    "ptrdiff_t" => 8,
    "dev_t" => 8,
    "int8_t" => 1,
    "uint8_t" => 1,
    "int16_t" => 2,
    "uint16_t" => 2,
    "int32_t" => 4,
    "uint32_t" => 4,
    "int64_t" => 8,
    "uint64_t" => 8,
    "int128_t" => 16,
    "uint128_t" => 16,
    "intptr_t" => 8,
    "uintptr_t" => 8,
    "ssize_t" => 8,
    "int_least8_t" => 1,
    "int_least16_t" => 2,
    "int_least32_t" => 4,
    "int_least64_t" => 8,
    "int_fast8_t" => 1,
    "int_fast16_t" => 8,
    "int_fast32_t" => 8,
    "int_fast64_t" => 8,
    "intmax_t" => 8,
    "sig_atomic_t" => 4,
    "wchar_t" => 4,
    "wint_t" => 4,
    "wctrans_t" => 8,
    "wctype_t" => 8,
    "_Bool" => 1,
    "long double" => 16,
    "float _Complex" => 8,
    "double _Complex" => 16,
    "long double _Complex" => 32,
    "__float128" => 16,
    "_Decimal32" => 4,
    "_Decimal64" => 8,
    "_Decimal128" => 16,
    "__float80" => 16,
  }.freeze

  LIMITS = {
    "FIXNUM_MAX" => 4611686018427387903,
    "FIXNUM_MIN" => -4611686018427387904,
    "CHAR_MAX" => 127,
    "CHAR_MIN" => -128,
    "SCHAR_MAX" => 127,
    "SCHAR_MIN" => -128,
    "UCHAR_MAX" => 255,
    "WCHAR_MAX" => 2147483647,
    "WCHAR_MIN" => -2147483648,
    "SHRT_MAX" => 32767,
    "SHRT_MIN" => -32768,
    "USHRT_MAX" => 65535,
    "INT_MAX" => 2147483647,
    "INT_MIN" => -2147483648,
    "UINT_MAX" => 4294967295,
    "LONG_MAX" => 9223372036854775807,
    "LONG_MIN" => -9223372036854775808,
    "ULONG_MAX" => 18446744073709551615,
    "LLONG_MAX" => 9223372036854775807,
    "LLONG_MIN" => -9223372036854775808,
    "ULLONG_MAX" => 18446744073709551615,
    "INT8_MAX" => 127,
    "INT8_MIN" => -128,
    "UINT8_MAX" => 255,
    "INT_LEAST8_MAX" => 127,
    "INT_LEAST8_MIN" => -128,
    "UINT_LEAST8_MAX" => 255,
    "INT_FAST8_MAX" => 127,
    "INT_FAST8_MIN" => -128,
    "UINT_FAST8_MAX" => 255,
    "INT16_MAX" => 32767,
    "INT16_MIN" => -32768,
    "UINT16_MAX" => 65535,
    "INT_LEAST16_MAX" => 32767,
    "INT_LEAST16_MIN" => -32768,
    "UINT_LEAST16_MAX" => 65535,
    "INT_FAST16_MAX" => 9223372036854775807,
    "INT_FAST16_MIN" => -9223372036854775808,
    "UINT_FAST16_MAX" => 18446744073709551615,
    "INT32_MAX" => 2147483647,
    "INT32_MIN" => -2147483648,
    "UINT32_MAX" => 4294967295,
    "INT_LEAST32_MAX" => 2147483647,
    "INT_LEAST32_MIN" => -2147483648,
    "UINT_LEAST32_MAX" => 4294967295,
    "INT_FAST32_MAX" => 9223372036854775807,
    "INT_FAST32_MIN" => -9223372036854775808,
    "UINT_FAST32_MAX" => 18446744073709551615,
    "INT64_MAX" => 9223372036854775807,
    "INT64_MIN" => -9223372036854775808,
    "UINT64_MAX" => 18446744073709551615,
    "INT_LEAST64_MAX" => 9223372036854775807,
    "INT_LEAST64_MIN" => -9223372036854775808,
    "UINT_LEAST64_MAX" => 18446744073709551615,
    "INT_FAST64_MAX" => 9223372036854775807,
    "INT_FAST64_MIN" => -9223372036854775808,
    "UINT_FAST64_MAX" => 18446744073709551615,
    "INTMAX_MAX" => 9223372036854775807,
    "INTMAX_MIN" => -9223372036854775808,
    "UINTMAX_MAX" => 18446744073709551615,
    "INTPTR_MAX" => 9223372036854775807,
    "INTPTR_MIN" => -9223372036854775808,
    "UINTPTR_MAX" => 18446744073709551615,
    "SIZE_MAX" => 18446744073709551615,
    "PTRDIFF_MAX" => 9223372036854775807,
    "PTRDIFF_MIN" => -9223372036854775808,
    "FLT_RADIX" => 2,
    "FLT_ROUNDS" => 1,
    "FLT_EVAL_METHOD" => 0,
    "FLT_MANT_DIG" => 24,
    "DBL_MANT_DIG" => 53,
    "LDBL_MANT_DIG" => 64,
    "FLT_DIG" => 6,
    "DBL_DIG" => 15,
    "LDBL_DIG" => 18,
    "FLT_MIN_EXP" => -125,
    "DBL_MIN_EXP" => -1021,
    "LDBL_MIN_EXP" => -16381,
    "FLT_MIN_10_EXP" => -37,
    "DBL_MIN_10_EXP" => -307,
    "LDBL_MIN_10_EXP" => -4931,
    "FLT_MAX_EXP" => 128,
    "DBL_MAX_EXP" => 1024,
    "LDBL_MAX_EXP" => 16384,
    "FLT_MAX_10_EXP" => 38,
    "DBL_MAX_10_EXP" => 308,
    "LDBL_MAX_10_EXP" => 4932,
    "FLT_DECIMAL_DIG" => 9,
    "DBL_DECIMAL_DIG" => 17,
    "LDBL_DECIMAL_DIG" => 21,
    "DECIMAL_DIG" => 21,
    "FLT_HAS_SUBNORM" => 1,
    "DBL_HAS_SUBNORM" => 1,
    "LDBL_HAS_SUBNORM" => 1,
    "FLT_MAX" => 3.4028234663852886e+38,
    "DBL_MAX" => 1.7976931348623157e+308,
    "FLT_EPSILON" => 1.1920928955078125e-07,
    "DBL_EPSILON" => 2.220446049250313e-16,
    "FLT_MIN" => 1.1754943508222875e-38,
    "DBL_MIN" => 2.2250738585072014e-308,
    "FLT_TRUE_MIN" => 1.401298464324817e-45,
    "DBL_TRUE_MIN" => 5.0e-324,
  }.freeze

  def self.ruby; File.join(CONFIG["bindir"], CONFIG["ruby_install_name"] + CONFIG["EXEEXT"]); end
end

# CRuby の rbconfig.rb はこれを **top-level** に置く。bundler の rubygems_ext.rb
# が `CROSS_COMPILING || RUBY_PLATFORM` と裸で参照するので、無いと gem の解決が
# NameError で止まる (i18n のテストが全滅していた)。
CROSS_COMPILING = nil unless defined?(CROSS_COMPILING)

# at_exit: register a block to run (in reverse order) when the program ends.  The
# C main loop drains $__at_exit after the top-level program returns.
$__at_exit = []
$__exit_trap = nil        # Signal.trap(:EXIT) handler; parked at the tail (runs first)
module Kernel
  # CRuby's rb_cmperr: the left operand is named by its class, the right one by
  # #inspect when it is an immediate or a Float.  `coercion` picks the Ruby 3.4
  # reason suffix (a refused numeric #coerce vs a nil <=>).
  def self.__cmperr(a, b, coercion: false)
    subject = case b
              when ::Float, ::Integer, ::Symbol then b.inspect
              else b.nil? || b.equal?(true) || b.equal?(false) ? b.inspect : b.class
              end
    raise ::ArgumentError, "comparison of #{a.class} with #{subject} failed: " \
                           "#{coercion ? 'coercion was not possible' : 'comparator returned nil'}"
  end

  # CRuby defines #tap in Ruby (internal:kernel), so it has a source_location and
  # its frame is labelled "Kernel#tap".  Keep it Ruby here for the same reason.
  def tap
    yield(self)
    self
  end

  def at_exit(&block)
    raise ArgumentError, "called without a block" unless block
    if $__exit_trap && $__at_exit.last.equal?($__exit_trap)
      $__at_exit.insert(-2, block)      # stay below the EXIT trap
    else
      $__at_exit << block
    end
    block
  end
  module_function :at_exit   # private Kernel#at_exit and public Kernel.at_exit, as in CRuby
end

# #pretty_inspect / #pretty_print exist only once pp is loaded (CRuby); Kernel#pp
# below loads it on first use, and mspec requires it for its failure formatter.

module Kernel
  # pp(*objs) — CRuby's Kernel#pp autoloads the real pretty-printer on first use
  # and then re-dispatches to the definition lib/pp.rb installs over this one.
  def pp(*objs)
    require 'pp'
    pp(*objs)
  end
  private :pp
end

module Process
  # The wait(2) status word behind $?.  `@raw` is the value waitpid filled in.
  class Status
    # Process::Status.wait(pid, flags = 0) — wait2 の Status だけを返す形。
    # CRuby と違い $? は変えない (呼び出し前の値を戻す)、子が居なければ
    # pid -1 の Status を返す。
    def self.wait(pid = -1, flags = 0)
      pid = pid.to_int if !pid.is_a?(Integer) && pid.respond_to?(:to_int)
      saved = $?
      begin
        _, st = Process.wait2(pid, flags)
        st
      rescue Errno::ECHILD
        Process.__mkstatus(-1, 0)
      ensure
        Process.send(:__set_last_status, saved)
      end
    end
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
  def self.exec(*a) = __exec(*a)
  def self.wait(*a) = __waitpid(*a)
  def self.waitpid(*a) = __waitpid(*a)
  def self.wait2(*a) = __waitpid2(*a)
  def self.waitpid2(*a) = __waitpid2(*a)
  def self.kill(*a) = __kill(*a)
  def self.getpgid(*a) = __getpgid(*a)
  def self.getpgrp = __getpgid(0)
  def self.last_status = $?
  def self.setpgid(pid, pgid) = __setpgid(__pid_arg(pid), __pid_arg(pgid))
  def self.__pid_arg(v)          # #to_int で coerce (CRuby と同じ)
    return v if v.is_a?(Integer)
    raise TypeError, "no implicit conversion of #{v.class} into Integer" unless v.respond_to?(:to_int)
    n = v.to_int
    raise TypeError, "can't convert #{v.class} into Integer" unless n.is_a?(Integer)
    n
  end
  def self.setsid = __setsid
  # module functions in CRuby; the Kernel ones are private so an explicit
  # `Process.exit` would otherwise be a NoMethodError
  def self.exit(*a) = Kernel.exit(*a)
  def self.exit!(*a) = Kernel.exit!(*a)
  def self.abort(*a) = Kernel.abort(*a)
  def self.daemon(nochdir = false, noclose = false) = __daemon(nochdir, noclose)
end

module Signal
  # Signals are reaped at interpreter check points (builtins/process.c) and
  # delivered here on the main thread.  Trapping a signal blocks it; an
  # untrapped HUP/INT/QUIT/TERM/ALRM/USR1/USR2 is caught only while the
  # process sleeps (a CPU-bound loop still dies by it, so Ctrl-C stays prompt).
  @@handlers = {}

  # Ruby installs its own handlers for these, so a program may not trap them.
  RESERVED__ = %w[SEGV BUS ILL FPE VTALRM].freeze
  # A signal nobody has trapped yet reports the OS disposition; the ones the
  # interpreter itself arms report "DEFAULT" (CRuby arms SIGINT at startup).
  # CRuby arms these at startup (so an untrapped one reports "DEFAULT"); the rest
  # are left to the OS and report "SYSTEM_DEFAULT" until something traps them.
  ARMED__ = %w[ALRM CHLD CLD HUP INT QUIT TERM USR1 USR2 ABRT PIPE SYS].freeze

  # The signal argument: Integer / String / Symbol, or an object with #to_str.
  # Deliberately NOT #to_int — CRuby refuses that.  Returns the short name.
  def self.__signame_arg(sig)
    if sig.is_a?(Integer)
      nm = signame(sig)
      raise ArgumentError, "invalid signal number (#{sig})" if nm.nil?
      return nm
    end
    if sig.is_a?(String) || sig.is_a?(Symbol)
      nm = sig.to_s
    elsif sig.respond_to?(:to_str)
      nm = sig.to_str
      raise ArgumentError, "bad signal type #{sig.class}" unless nm.is_a?(String)
    else
      raise ArgumentError, "bad signal type #{sig.class}"
    end
    raise ArgumentError, "negative signal name: #{nm}" if nm.start_with?("-")
    short = nm.start_with?("SIG") ? nm[3..] : nm
    return short if short == "EXIT"
    raise ArgumentError, "unsupported signal 'SIG#{short}'" if list[short].nil?
    short
  end

  # The command argument, normalized to what #trap stores and hands back:
  # a callable, nil (ignore), or one of "DEFAULT" / "IGNORE" / "SYSTEM_DEFAULT".
  def self.__command_arg(command, blk)
    return blk if command.nil? && blk
    return nil if command.nil?
    if command.is_a?(Symbol) || command.is_a?(String)
      case command.to_s
      when "SIG_DFL", "DEFAULT"     then return "DEFAULT"
      when "SIG_IGN", "IGNORE"      then return "IGNORE"
      when "SYSTEM_DEFAULT"         then return "SYSTEM_DEFAULT"
      when "EXIT"                   then return "EXIT"
      end
    end
    command
  end

  def self.trap(sig, command = nil, &blk)
    short = __signame_arg(sig)
    raise ArgumentError, "can't trap reserved signal: SIG#{short}" if RESERVED__.include?(short)
    if short == "KILL" || short == "STOP"
      raise Errno::EINVAL, "SIG#{short}"                          # man 2 signal: not catchable
    end
    h = __command_arg(command, blk)
    prev = @@handlers.key?(short) ? @@handlers[short] : (ARMED__.include?(short) ? "DEFAULT" : "SYSTEM_DEFAULT")
    @@handlers[short] = h
    if short == "EXIT"
      # CRuby keeps the EXIT trap in a separate list that runs before every
      # at_exit block.  Here it is parked at the tail of $__at_exit (the drain
      # pops from the end) and Kernel#at_exit inserts underneath it.
      $__at_exit.pop if $__exit_trap && $__at_exit.last.equal?($__exit_trap)
      $__exit_trap = h.respond_to?(:call) ? h : nil
      $__at_exit << $__exit_trap if $__exit_trap
      return prev
    end
    n = list[short]
    if n && n > 0
      if h == "IGNORE"
        __signal_trap(n, "IGNORE")     # a blocked+ignored signal is discarded by the kernel
        __signal_block(n, false)
      elsif h == "SYSTEM_DEFAULT"
        __signal_trap(n, "DEFAULT")    # hand it back to the OS (only SYSTEM_DEFAULT does)
        __signal_block(n, false)
      else                             # "DEFAULT" means *Ruby's* default: raise
                                       # SignalException/Interrupt at a check point
        __signal_trap(n, "DEFAULT")    # undo a previous SIG_IGN; blocked, so it just stays pending
        __signal_block(n, true)        # deliver it ourselves at the next check point
      end
    end
    prev
  end

  # Called from the interpreter with a signal number just reaped from the
  # pending set.  Runs the trap handler, or raises for the default disposition.
  def self.__deliver(signo)
    nm = signame(signo)
    # never trapped → Ruby's default disposition (raise), which is NOT the same
    # as trap(sig, nil), whose stored nil means "ignore"
    unless @@handlers.key?(nm)
      raise(signo == Signal.list["INT"] ? Interrupt.new("") : SignalException.new(signo))
    end
    case (h = @@handlers[nm])
    when "IGNORE", "SIG_IGN" then nil
    when "EXIT"              then exit(0)
    when nil                 then nil          # trap(sig, nil) → ignore
    when "DEFAULT", "SIG_DFL", "SYSTEM_DEFAULT"
      raise(signo == Signal.list["INT"] ? Interrupt.new("") : SignalException.new(signo))
    else
      __trap_ctx(true)   # Mutex#lock refuses inside the handler (CRuby's trap context)
      begin
        h.call(signo)   # a non-callable handler is a NoMethodError at the point of use (CRuby)
      ensure
        __trap_ctx(false)
      end
    end
  end

  def self.signame(signo) = __signal_signame(signo)
  def self.signo(sig) = sig.is_a?(Integer) ? sig : __signal_signo(sig)

  def self.list
    h = {}
    (1..31).each { |n| (nm = __signal_signame(n)) && h[nm] = n }
    h["EXIT"] = 0
    h["CLD"] = h["CHLD"] if h.key?("CHLD")     # historical aliases CRuby keeps
    h["IOT"] = h["ABRT"] if h.key?("ABRT")
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
  # The default #respond_to_missing? — always false.  Defining it (rather than
  # leaving the name unbound) is what makes `Kernel.private_instance_methods`
  # list it and lets a mock replace it; the C fast paths skip a dispatch when the
  # definition they find is this one.
  def respond_to_missing?(name, include_private = false)
    false
  end
  private :respond_to_missing?

  # Ruby 4.x: the default #inspect asks this which ivars to show.  nil = all,
  # which is what the default answers; the C side only dispatches when the
  # definition it finds is NOT this one.
  def instance_variables_to_inspect = nil
  private :instance_variables_to_inspect

  def trap(sig, command = nil, &blk) = Signal.trap(sig, command, &blk)
  module_function :trap

  # test(cmd, file[, file2]) — the file predicates, spelled as a character.
  def test(cmd, file1, file2 = nil) = __process_test(cmd, File.path(file1), file2 && File.path(file2))
  module_function :test

  def select(*args) = IO.select(*args)
  module_function :select

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

  # 実体は Object 側の C メソッドで、そちらが先に見つかる。ここに名前を置くのは
  # Kernel.private_instance_methods に並べるためと、Kernel.system の形で呼べるため。
  def system(*args, **opts, &blk); __system(*args, **opts, &blk); end
  def spawn(*args, **opts, &blk);  __spawn(*args, **opts, &blk);  end
  def exec(*args, **opts, &blk);   __exec(*args, **opts, &blk);   end
  module_function :system, :spawn, :exec
end

class Object
  # The Method object for a singleton method, or NameError when the method is
  # not defined on the singleton class itself.
  def singleton_method(name)
    sc = singleton_class
    sym = name.to_sym
    m = begin
          method(sym)
        rescue NameError
          nil
        end
    # nil/true/false answer #singleton_class with NilClass/TrueClass/FalseClass,
    # which are ordinary classes -- so they never own a singleton method, however
    # `def nil.foo` looks.  Without this the ancestor walk below accepts them.
    if m && sc.singleton_class?
      # The singleton class *and* the modules included/prepended into it count;
      # the inherited part of the chain (from the first other Class on) does not.
      sc.ancestors.each do |mod|
        break if mod.is_a?(Class) && !mod.equal?(sc)
        return m if mod.equal?(m.owner)
      end
    end
    raise NameError.new("undefined singleton method '#{name}' for '#{inspect}'", sym)
  end
end

# File / FileTest predicates on top of the __process_test character commands
# (the same primitive Kernel#test uses) and __mode_bits.
class File
  # A stat compares (and so ==) by mtime; CRuby mixes Comparable in for that.
  class Stat
    include Comparable

    def inspect
      s = +"#<File::Stat dev=0x#{dev.to_s(16)}, ino=#{ino}, mode=#{sprintf("%07o", mode)}, nlink=#{nlink}"
      s << ", uid=#{uid}, gid=#{gid}, rdev=0x#{rdev.to_s(16)}, size=#{size}, blksize=#{blksize.inspect}"
      s << ", blocks=#{blocks.inspect}, atime=#{atime.inspect}, mtime=#{mtime.inspect}, ctime=#{ctime.inspect}"
      s << ">"
    end
  end

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
    alias_method :zero?, :empty?      # CRuby: File.zero? IS File.empty?
    def identical?(a, b)
      if a.is_a?(IO) || b.is_a?(IO)         # an open file is fstat'ed: it may have been renamed (Logger's shift)
        sa = a.is_a?(IO) ? a.stat : File.stat(File.path(a))
        sb = b.is_a?(IO) ? b.stat : File.stat(File.path(b))
        return sa.dev == sb.dev && sa.ino == sb.ino
      end
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
    # any of the process's groups (File::Stat#grpowned? knows the supplementary ones)
    def grpowned?(path)  = (File.stat(File.path(path)).grpowned? rescue false)
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
  %i[empty? zero? identical? pipe? socket? blockdev? chardev? sticky? setuid? setgid?
     owned? grpowned? readable_real? writable_real? executable_real?
     world_readable? world_writable?].each do |m|
    define_method(m) { |*a| File.send(m, *a) }
    module_function m
  end
end

# Dir conveniences on top of the C primitives (entries/children/chdir/…).
class Dir
  include Enumerable            # CRuby: Dir is Enumerable over #each
  def self.home(user = nil)
    # a fresh, unfrozen String (ENV[] may hand back a frozen one)
    if user.nil? || user == ""
      h = ENV["HOME"]
      return h.nil? ? __passwd_home(nil) : (+h.dup)
    end
    # /etc/passwd lookup, the portable-enough way.
    File.foreach("/etc/passwd") do |line|
      f = line.split(":")
      return f[5] if f[0] == user
    end rescue nil
    raise ArgumentError, "user #{user} doesn't exist"
  end

  # Ruby 4.1: #children paired with each entry's File.ftype.  The lstat path is
  # built from BINARY copies so a non-UTF-8 entry name still joins cleanly.
  def self.scan(path, **opts, &blk)
    dir = File.path(path)
    pairs = children(dir, **opts).map { |n| [n, __entry_type(dir, n)] }
    return pairs unless blk
    pairs.each { |n, t| blk.call(n, t) }
    nil
  end

  def self.__entry_type(dir, name)
    File.ftype(File.join(dir.b, name.b)).to_sym
  rescue SystemCallError                  # vanished between readdir and lstat
    :unknown
  end

  def scan(&blk)
    dir = path
    pairs = children.map { |n| [n, Dir.__entry_type(dir, n)] }
    return pairs unless blk
    pairs.each { |n, t| blk.call(n, t) }
    nil
  end

  # Dir.chdir with no argument goes to the home directory (CRuby)
  def self.chdir(path = nil, &blk)
    __chdir(path.nil? ? home : path, &blk)
  end

  # `encoding:` only names the encoding of the returned strings, so it is
  # accepted and forwarded rather than being an arity error.
  def self.foreach(path, **opts, &blk)
    return to_enum(:foreach, path, **opts) unless blk
    entries(path, **opts).each(&blk)
    nil
  end

  def self.each_child(path, **opts, &blk)
    return to_enum(:each_child, path, **opts) unless blk
    children(path, **opts).each(&blk)
    nil
  end

  # a path that is not a directory is simply "not empty" (CRuby returns false
  # rather than raising ENOTDIR)
  def self.empty?(path)
    p = File.path(path)
    raise Errno::ENOENT, p unless File.exist?(p)
    return false unless File.directory?(p)
    children(p).empty?
  end

  # the current user's home from the password database, used when HOME is unset
  def self.__passwd_home(user)
    name = user || (Etc.getlogin rescue nil)
    File.foreach("/etc/passwd") do |line|
      f = line.split(":")
      return f[5] if name ? f[0] == name : f[2].to_i == Process.uid
    end rescue nil
    raise ArgumentError, "couldn't find HOME environment -- expanding `~'"
  end

  def each_child(&blk)
    return to_enum(:each_child) unless blk
    children.each(&blk)
    self
  end

  alias_method :__each_raw, :each
  private :__each_raw
  def each(&blk)
    return to_enum(:each) { nil } unless blk
    __each_raw(&blk)
  end

  # CRuby 3.3+: Dir#chdir — change into this directory by its descriptor; the
  # block form returns to where it was even if that path no longer exists.
  def chdir(&blk) = Dir.fchdir(fileno, &blk)

  # dirfd(3) equivalent: an fd opened on the directory, created lazily and
  # owned by this Dir (closed with GC; koruby Dir has no explicit close of it).
  def fileno
    raise IOError, "closed directory" if @__dir_closed
    @__dir_fd ||= IO.sysopen(path, File::RDONLY)
  end
end

module Process
  # groups/waitall などの薄い実装 (koruby は setgroups 系を持たないので
  # 取得のみ、変更は空実装)。
  def self.groups; __getgroups; end
  def self.groups=(v); __setgroups(v); end
  def self.maxgroups; @__maxgroups || 65536; end
  def self.maxgroups=(v); @__maxgroups = v.to_int; v; end
  def self.initgroups(user, group) = __initgroups(user.to_s, __pid_arg(group))
  def self.setproctitle(title); title.to_s; end
  def self.getsid(pid = 0); __getpgid(pid); end
  def self.warmup; true; end
  # waitall — 全子プロセスを回収して [[pid, status], ...] を返す
  def self.waitall
    res = []
    loop do
      begin
        pid, st = Process.wait2
      rescue Errno::ECHILD
        break
      end
      break if pid.nil?
      res << [pid, st]
    end
    res
  end
end

class Regexp
  # Regexp.linear_time? — CRuby は「位置と状態のメモ化で線形時間にできるか」を
  # 答える。メモ化で潰せないのは後方参照 (\1 / \k<n>) と subexpression call
  # (\g<n>) だけで、先読み・後読みは linear。
  # 注意: astrogre はメモ化を持たないバックトラック式なので、koruby が true を
  # 返しても実際の実行が線形とは限らない。CRuby と同じ答えを返す互換のための
  # 述語であって、この実装の保証ではない。
  def self.linear_time?(re, opts = nil)
    if re.is_a?(Regexp)
      warn "warning: flags ignored" if opts && !$VERBOSE.nil?
      src = re.source
    elsif re.is_a?(String)
      src = re
    else
      return false
    end
    return false if src.match?(/\\[0-9]|\\k<|\\g<|\(\?~/)
    !__lookahead_captures?(src)
  end
  # A capturing group inside a lookahead (?= / (?! defeats the memoization;
  # inside a lookbehind it does not (what CRuby 4.1 answers).
  def self.__lookahead_captures?(src)
    stack = []   # one entry per open group: true when it is (or is inside) a lookahead
    i = 0
    while i < src.size
      ch = src[i]
      if ch == "\\"
        i += 2; next
      elsif ch == "["                       # skip a character class (nested [ ] and escapes)
        depth = 1; i += 1
        while i < src.size && depth > 0
          if src[i] == "\\" then i += 1
          elsif src[i] == "[" then depth += 1
          elsif src[i] == "]" then depth -= 1
          end
          i += 1
        end
        next
      elsif ch == "("
        inside = stack.last || false
        rest = src[i + 1, 3] || ""
        if rest.start_with?("?=", "?!")
          stack << true
        elsif rest.start_with?("?<=", "?<!")
          stack << false                     # a lookbehind resets it
        elsif rest.start_with?("?")
          return true if inside && (rest.start_with?("?<") || rest.start_with?("?'"))   # named capture
          stack << inside
        else
          return true if inside                # plain capturing group
          stack << inside
        end
      elsif ch == ")"
        stack.pop
      end
      i += 1
    end
    false
  end
  private_class_method :__lookahead_captures?
  def self.timeout; @__timeout; end
  def self.timeout=(sec)
    if sec.nil?
      @__timeout = nil
    else
      f = Float(sec)
      raise ArgumentError, "invalid timeout: #{sec}" if f <= 0
      @__timeout = f
    end
    sec
  end
  def self.try_convert(obj)
    return obj if obj.is_a?(Regexp)
    return nil unless obj.respond_to?(:to_regexp)
    r = obj.to_regexp
    raise TypeError, "can't convert #{obj.class} into Regexp (#{obj.class}#to_regexp gives #{r.class})" unless r.is_a?(Regexp) || r.nil?
    r
  end
end

class Fiber
  # The Fiber scheduler is per thread (CRuby).  Kernel#sleep and the backtick's pipe
  # read in a non-blocking fiber go through it; Mutex / Queue / IO in general do not yet.
  def self.scheduler = Thread.current.instance_variable_get(:@__fiber_scheduler)
  # nil in a blocking fiber (the root, or Fiber.new(blocking: true))
  def self.current_scheduler = blocking? ? nil : scheduler
  def self.set_scheduler(sched)
    unless sched.nil?
      %i[block unblock kernel_sleep io_wait fiber_interrupt].each do |m|
        raise ArgumentError, "Scheduler must implement ##{m}" unless sched.respond_to?(m)
      end
    end
    th = Thread.current
    old = th.instance_variable_get(:@__fiber_scheduler)
    if old                                 # the previous one runs to completion first
      begin
        __scheduler_close(old)
      ensure
        th.instance_variable_set(:@__fiber_scheduler, nil)
      end
    end
    th.instance_variable_set(:@__fiber_scheduler, sched)
  end
  # #scheduler_close first: a legacy #close may itself call set_scheduler(nil)
  def self.__scheduler_close(s)
    if s.respond_to?(:scheduler_close, true) then s.__send__(:scheduler_close)
    elsif s.respond_to?(:close, true) then s.__send__(:close)
    end
  end
  def self.__thread_end
    set_scheduler(nil) if scheduler
  end
  def self.schedule(*args, **kw, &blk)
    s = scheduler                          # the thread's, even from a blocking fiber
    raise RuntimeError, "No scheduler is available!" if s.nil?
    s.fiber(*args, **kw, &blk)
  end
end

module Kernel
  # `cmd` in a non-blocking fiber: read the pipe through scheduler.io_wait so other
  # fibers run meanwhile (C's #` sends here only when a scheduler is set)
  private def __sched_backtick(sched, cmd)
    cmd = cmd.to_str
    r, w = IO.pipe
    begin
      pid = Process.spawn(cmd, out: w)
    ensure
      w.close
    end
    out = +""
    begin
      loop do
        chunk = r.read_nonblock(65536, exception: false)
        case chunk
        when :wait_readable then sched.io_wait(r, IO::READABLE, nil)
        when nil then break
        else out << chunk
        end
      end
    ensure
      r.close
      Process.wait(pid)
    end
    out.force_encoding(Encoding.default_external)
  end
end

class Regexp
  # fixed_encoding? — /u /e /s のような encoding 指定が付いているか、あるいは
  # パターン自体が 7bit ASCII に収まらない (非 ASCII バイト・\xNN >= 0x80 の
  # エスケープ・ASCII 非互換の source encoding) 場合。CRuby はこの場合も
  # エンコーディングを固定する。
  def fixed_encoding?
    src = __source_raw                    # #source asks #encoding, which asks here
    return false if src.nil?              # Regexp.allocate: no pattern yet (CRuby answers false)
    return true if (options & Regexp::FIXEDENCODING) != 0
    return true unless src.encoding.ascii_compatible?
    return true if __utf8_escape?(src)    # \u{8768} pins the pattern to UTF-8
    return true unless src.ascii_only?
    src.scan(/\\x([0-9a-fA-F]{1,2})/).any? { |h| h[0].to_i(16) > 0x7f }
  end

  # Regexp.new/.compile build the object outright, so #initialize only ever runs
  # as `super` from a Regexp subclass's own #initialize — on the uninitialized
  # payload .new handed it.  Anything else is CRuby's "already initialized".
  def initialize(*args)
    raise FrozenError.new("can't modify frozen Regexp: #{inspect}", receiver: self) if frozen?
    __init_pattern(*args)
  end
  private :initialize
end

# RubyVM: koruby has no YJIT / ZJIT, but CRuby code asks about them without a
# defined? guard (csv's test_general: `RubyVM.const_defined?("YJIT")`).  Only the
# predicates exist.  AbstractSyntaxTree is CRuby's prism-mode face: .of refuses
# (as CRuby does for prism-compiled code), .parse has no parse.y to give.
module RubyVM
  module AbstractSyntaxTree
    class Node; end   # never made: pp reopens it when AbstractSyntaxTree is defined
    def self.of(_body, keep_script_lines: false, error_tolerant: false, keep_tokens: false)
      raise RuntimeError, "cannot get AST for ISEQ compiled by prism"
    end
    def self.parse(*, **) = raise(NotImplementedError, "RubyVM::AbstractSyntaxTree.parse: koruby has no parse.y AST (use Prism)")
    def self.parse_file(*, **) = raise(NotImplementedError, "RubyVM::AbstractSyntaxTree.parse_file: koruby has no parse.y AST (use Prism)")
    # CRuby keeps the node id of every backtrace position; koruby's backtrace has the
    # line only, so error_highlight's id is found again in the file with Prism: the
    # call / constant on that line named like the exception (NameError), else the only
    # call there.  nil when that does not single one out (no highlight, never a wrong one).
    def self.node_id_for_backtrace_location(loc)
      path = loc.absolute_path
      return nil unless path && File.file?(path)
      line = loc.lineno
      exc = loc.instance_variable_get(:@__exc)
      name = exc.name if exc.is_a?(NameError)
      require "prism"
      on_line = []
      stack = [Prism.parse_file(path).value]
      until stack.empty?
        n = stack.pop
        next unless n.location.start_line <= line && line <= n.location.end_line
        on_line << n if n.location.start_line == line
        stack.concat(n.compact_child_nodes)
      end
      picks = on_line.select { |n| n.is_a?(Prism::CallNode) || (name && n.class.name.include?("Constant")) }
      picks = picks.select { |n| n.respond_to?(:name) && n.name == name } if name
      # a call whose BLOCK holds another candidate is not the one that failed (`t { 1 + nil }`);
      # one nested in the ARGUMENTS stays ambiguous
      if picks.size > 1
        picks = picks.reject do |c|
          blk = c.is_a?(Prism::CallNode) && c.block
          blk && picks.any? { |o| !o.equal?(c) && blk.location.start_offset <= o.location.start_offset && o.location.end_offset <= blk.location.end_offset }
        end
      end
      if picks.empty?                     # the callee's own line (ArgumentError): its def
        m = loc.base_label.to_s.sub(/\A.*[#.]/, "")
        picks = on_line.select { |n| n.is_a?(Prism::DefNode) && n.name.to_s == m }
      end
      picks.size == 1 ? picks[0].node_id : nil
    end
  end

  # Only compile-as-syntax-check and #eval (state_machines validates eval strings
  # with it because RUBY_ENGINE is "ruby").  No binary form: to_binary /
  # load_from_binary stay undefined so bootsnap-style probes keep their fallback.
  class InstructionSequence
    def self.compile(source, file = "<compiled>", path = nil, line = 1, _options = nil)
      src = source.respond_to?(:to_io) ? source.to_io.read : source.to_str
      __check_syntax(src, file, line)
      new_iseq__(src, file, path || file, line)
    end
    def self.new(...) = compile(...)
    def self.compile_file(path, _options = nil)
      compile(File.read(path), path, File.expand_path(path), 1)
    end
    def self.of(_body) = nil
    def self.__check_syntax(src, file, line)   # a BEGIN that throws runs before anything else
      catch(:__korb_iseq_syntax_ok) { __fresh_binding.eval("BEGIN{throw :__korb_iseq_syntax_ok}\n#{src}", file, line - 1) }
    end
    def self.__fresh_binding = binding
    def self.new_iseq__(src, file, path, line)
      i = allocate
      i.instance_variable_set(:@src, src); i.instance_variable_set(:@file, file)
      i.instance_variable_set(:@path, path); i.instance_variable_set(:@line, line)
      i
    end
    private_class_method :__check_syntax, :__fresh_binding, :new_iseq__

    def eval = TOPLEVEL_BINDING.eval(@src, @path, @line)
    def path = @file
    def absolute_path = @path
    def label = "<compiled>"
    def base_label = "<compiled>"
    def first_lineno = @line
    def inspect = "<RubyVM::InstructionSequence:#{label}@#{@file}:#{@line}>"
    # CRuby's SimpleDataFormat header only (no bytecode): ActionView reads misc[:parser]
    def to_a
      ["YARVInstructionSequence/SimpleDataFormat", 4, 1, 1, { arg_size: 0, local_size: 0, stack_max: 1, parser: :prism },
       label, @file, @path, @line, :top, [], {}, [], []]
    end
  end
  module YJIT
    def self.enabled? = false
    def self.stats_enabled? = false
    def self.log_enabled? = false
    def self.trace_exit_locations_enabled? = false
  end
  module ZJIT
    def self.enabled? = false
    def self.stats_enabled? = false
    def self.trace_exit_locations_enabled? = false
  end
end

# Ruby 3.5+: RUBY_* 定数の Module 名前空間ミラー。
module Ruby
  VERSION        = RUBY_VERSION
  PATCHLEVEL     = RUBY_PATCHLEVEL
  COPYRIGHT      = RUBY_COPYRIGHT
  DESCRIPTION    = RUBY_DESCRIPTION
  ENGINE         = RUBY_ENGINE
  ENGINE_VERSION = RUBY_ENGINE_VERSION
  PLATFORM       = RUBY_PLATFORM
  RELEASE_DATE   = RUBY_RELEASE_DATE
  REVISION       = RUBY_REVISION

  # Ruby 4.1: the source extent of a method, block or lambda.  Lines are 1-based
  # and columns are 0-based BYTE offsets into their line; end_column is exclusive.
  # Built only by #source_range, so there is no public constructor (as in CRuby).
  class SourceRange
    attr_reader :path, :absolute_path, :start_line, :start_column, :end_line, :end_column

    class << self
      # Instances come only from #source_range, so CRuby leaves this class with
      # neither a constructor nor an allocator.  __alloc keeps the real one.
      alias_method :__alloc, :allocate
      private :__alloc

      # CRuby undefines the constructor and leaves #allocate raising.  koruby's
      # undef_method does not reach an inherited `new`, so shadow it instead —
      # same exception, but it stays visible in .methods.
      def new(*) = raise(NoMethodError, "undefined method 'new' for class Ruby::SourceRange")
      def allocate = raise(TypeError, "allocator undefined for Ruby::SourceRange")

      # info = [path, start_line, start_column, end_line, end_column], or nil.
      def __build(info)
        return nil unless info
        r = __alloc
        r.__send__(:__init, info)
        r
      end
      private :__build
    end

    def inspect
      "#<Ruby::SourceRange #{@absolute_path || @path}:(#{@start_line},#{@start_column})-(#{@end_line},#{@end_column})>"
    end

    private def __init(info)
      @path = info[0]
      # nil for a name no file was ever read under (an eval filename, -e), which
      # is the same rule Thread::Backtrace::Location#absolute_path follows.
      @absolute_path =
        if @path.empty? || @path.start_with?('<') || @path == '-e'
          nil
        else
          __source_abs_path(@path)
        end
      @start_line   = info[1]
      @start_column = info[2]
      @end_line     = info[3]
      @end_column   = info[4]
    end
  end
end

# Random::Formatter is a core module in CRuby (random.c): rand / random_number on
# top of the receiver's #bytes, included and extended by Random.  The vendored
# lib/random/formatter.rb reopens it and adds hex / alphanumeric / uuid.
class Random
  module Formatter
    def random_number(n = 0) = __fmt_rand(n, false)
    def rand(n = 0) = __fmt_rand(n, true)

    private def __fmt_float = (bytes(8).unpack1("Q<") >> 11) * (1.0 / (1 << 53))

    private def __fmt_int(n)
      nb = (n.bit_length + 7) / 8
      mask = (1 << n.bit_length) - 1
      loop do
        v = 0
        bytes(nb).each_byte { |b| v = (v << 8) | b }
        v &= mask
        return v if v < n
      end
    end

    private def __fmt_rand(n, strict)
      case n
      when nil then return __fmt_float
      when Range
        lo, hi = n.begin, n.end
        if lo.is_a?(Integer) && hi.is_a?(Integer)
          span = hi - lo + (n.exclude_end? ? 0 : 1)
          return span > 0 ? lo + __fmt_int(span) : nil
        end
        span = hi.to_f - lo.to_f
        return nil if span < 0
        return lo + __fmt_float * span
      when Float
        return n * __fmt_float if n > 0
      when Numeric
        i = n.to_int
        return __fmt_int(i) if i > 0
        return __fmt_float if i == 0
      end
      raise ArgumentError, "invalid argument - #{n.inspect}" if strict && !(n.is_a?(Numeric) && n == 0)
      __fmt_float
    end
  end
  include Formatter
  extend Formatter
end

# Random's class-level RNG functions are public singleton methods in CRuby
# (Kernel#rand / #srand are private instance methods, so they are not reachable
# through `Random.rand`).
class Random
  class << self
    def srand(*args) = Kernel.srand(*args)
    def seed         = @__seed || Kernel.srand(Kernel.srand)
    # new_seed is the C Random.new_seed (fresh OS entropy, independent of srand)
    def bytes(n)     = Random.new(Kernel.srand(Kernel.srand)).bytes(n)
    # Like #rand, except that 0 (and a 0.0 max) means "no bound" rather than
    # "the whole Float range is out" — Random::Formatter's entry point.
    def random_number(n = 0) = (n == 0 ? rand : rand(n))
  end

  def random_number(n = 0) = (n == 0 ? rand : rand(n))

  # Two generators are equal when they would produce the same sequence: same
  # class, same seed and same internal state (what Marshal round-trips).
  def ==(other)
    return false unless other.is_a?(Random)
    return false unless self.class == other.class
    # #b on both: the state is a byte blob, and String#== would call two equal
    # states different when their encoding tags differ (a rebuilt one is binary).
    seed == other.seed &&
      instance_variable_get(:@__mt).b == other.instance_variable_get(:@__mt).b
  end
  alias eql? ==

  # Marshal: CRuby dumps [state, left, seed], where state is the 624 MT words as
  # one Integer (word 0 lowest) and `left` counts the words not yet consumed
  # since the last twist — its `next` cursor sits at state + MT_N + 1 - left,
  # which is exactly the index @__mt keeps as its trailing word.
  MT_N = 624
  private_constant :MT_N

  def marshal_dump
    w = instance_variable_get(:@__mt).unpack("L*")
    n = 0
    (MT_N - 1).downto(0) { |i| n = (n << 32) | w[i] }
    [n, MT_N + 1 - w[MT_N], seed]
  end

  def marshal_load(ary)
    n = ary[0].to_i
    w = Array.new(MT_N) { v = n & 0xffffffff; n >>= 32; v }
    w << MT_N + 1 - ary[1].to_i
    instance_variable_set(:@__mt, w.pack("L*"))
    instance_variable_set(:@__seed, ary[2])
    self
  end
end

# ENV — the C side (builtins/env.c) provides the primitives; the Hash-shaped
# conveniences and the argument checking live here.
class << ENV
  # A name/value must be a String (or #to_str-able); a name may be neither
  # empty nor contain "=" (setenv(3) rejects those).
  private def __env_name(k)
    unless k.is_a?(String)
      raise TypeError, "no implicit conversion of #{k.nil? ? 'nil' : k.class} into String" unless k.respond_to?(:to_str)
      k = k.to_str
      raise TypeError, "no implicit conversion into String" unless k.is_a?(String)
    end
    raise Errno::EINVAL, "setenv(#{k.inspect})" if k.empty? || k.include?("=")
    k
  end
  private def __env_value(v)
    return v if v.is_a?(String)
    raise TypeError, "no implicit conversion of #{v.nil? ? 'nil' : v.class} into String" unless v.respond_to?(:to_str)
    s = v.to_str
    raise TypeError, "no implicit conversion into String" unless s.is_a?(String)
    s
  end

  def to_a = to_h.to_a
  def dig(*args) = to_h.dig(*args)
  def rehash = nil
  def freeze = self
  def first(n = nil) = n.nil? ? to_a.first : to_a.first(n)
  def any?(&b) = b ? to_h.any?(&b) : !empty?
  def none?(&b) = to_h.none?(&b)
  def all?(&b) = to_h.all?(&b)
  def count(*a, &b) = a.empty? && b.nil? ? size : to_h.count(*a, &b)
  def find(&b) = b ? to_h.find(&b) : __to_enum_sized(:find)
  def filter_map(&b) = b ? to_h.filter_map(&b) : __to_enum_sized(:filter_map)
  def sum(init = 0, &b) = to_h.sum(init, &b)
  def min_by(&b) = b ? to_h.min_by(&b) : __to_enum_sized(:min_by)
  def max_by(&b) = b ? to_h.max_by(&b) : __to_enum_sized(:max_by)
  def sort_by(&b) = b ? to_h.sort_by(&b) : __to_enum_sized(:sort_by)
  def group_by(&b) = b ? to_h.group_by(&b) : to_enum(:group_by)
  def partition(&b) = b ? to_h.partition(&b) : to_enum(:partition)
  def flat_map(&b) = b ? to_h.flat_map(&b) : to_enum(:flat_map)
  def zip(*a, &b) = to_h.zip(*a, &b)
  def sort(&b) = to_h.sort(&b)
  def map(&b) = b ? to_h.map(&b) : to_enum(:map)
  def collect(&b) = map(&b)
  def each_entry(&b) = b ? (to_h.each { |kv| b.call(kv) }; self) : to_enum(:each_entry)
  def each_with_index(&b) = b ? (to_h.each_with_index { |kv, i| b.call(kv, i) }; self) : to_enum(:each_with_index)
  def each_with_object(o, &b) = b ? to_h.each_with_object(o, &b) : to_enum(:each_with_object, o)
  def reduce(*a, &b) = to_h.reduce(*a, &b)
  def inject(*a, &b) = to_h.inject(*a, &b)

  # ENV.shift removes and returns the first pair (nil when empty).
  def shift
    k = keys.first
    return nil if k.nil?
    v = self[k]
    delete(k)
    [k, v]
  end

  # ENV.replace validates the WHOLE hash before touching the environment, so a
  # bad entry leaves it untouched ("does not accept good data following an error").
  def replace(other)
    h = other.is_a?(Hash) ? other : (other.respond_to?(:to_hash) ? other.to_hash : nil)
    raise TypeError, "no implicit conversion of #{other.class} into Hash" unless h.is_a?(Hash)
    pairs = h.map { |k, v| [__env_name(k), __env_value(v)] }
    clear
    pairs.each { |k, v| self[k] = v }
    self
  end

  # A value that is neither a String nor #to_str-convertible answers nil (CRuby).
  def value?(v)
    v = __env_value(v) unless v.is_a?(String)
    values.include?(v)
  rescue TypeError
    nil
  end
  alias has_value? value?    # a real alias: ENV.method(:has_value?) == ENV.method(:value?)
  # Ruby 4.1: Hash#fetch_values for ENV — a missing name raises KeyError unless
  # a block supplies its replacement.  ENV.fetch does the name coercion.
  def fetch_values(*names, &blk)
    names.map { |name| blk ? fetch(name, &blk) : fetch(name) }
  end

  def inspect = to_h.inspect
end

class << ENV
  # select / reject and their ! forms return an Enumerator when no block is given.
  def select(&b) = b ? __select(&b) : __to_enum_sized(:select)
  def reject(&b) = b ? __reject(&b) : __to_enum_sized(:reject)
  def select!(&b) = b ? __select!(&b) : __to_enum_sized(:select!)
  def reject!(&b) = b ? __reject!(&b) : __to_enum_sized(:reject!)
  alias filter select      # a real alias: ENV.method(:filter) == ENV.method(:select)
  alias filter! select!
  # each / each_pair likewise yield an Enumerator when block-less.
  alias __each each
  def each(&b) = b ? __each(&b) : __to_enum_sized(:each)
  alias each_pair each
  alias __each_key each_key
  alias __each_value each_value
  def each_key(&b) = b ? __each_key(&b) : __to_enum_sized(:each_key)
  def each_value(&b) = b ? __each_value(&b) : __to_enum_sized(:each_value)
  alias __keep_if keep_if
  alias __delete_if delete_if
  def keep_if(&b) = b ? __keep_if(&b) : __to_enum_sized(:keep_if)
  def delete_if(&b) = b ? __delete_if(&b) : __to_enum_sized(:delete_if)
  # ENV is not copyable (CRuby raises rather than handing out a broken twin).
  def clone(freeze: nil)
    unless freeze.nil? || freeze == true || freeze == false
      raise ArgumentError, "unexpected value for freeze: #{freeze.class}"
    end
    raise TypeError, "Cannot clone ENV, use ENV.to_h to get a copy of ENV as a hash"
  end
  def dup
    raise TypeError, "Cannot dup ENV, use ENV.to_h to get a copy of ENV as a hash"
  end
  alias __delete delete
  # ENV.delete calls the block with the name when the variable is absent.
  def delete(name)
    name = name.to_str if !name.is_a?(String) && name.respond_to?(:to_str)   # coerce once (key? + __delete would ask twice)
    had = key?(name)
    r = __delete(name)
    return yield(name) if !had && block_given?
    r
  end
end

# $LOAD_PATH.resolve_feature_path(feature) — what `require feature` would load,
# as [:rb | :so, absolute path], without loading it.  nil when not found.
def $LOAD_PATH.resolve_feature_path(feature)
  f = feature.is_a?(String) ? feature : feature.to_str
  cands = f.end_with?(".rb", ".so") ? [f] : ["#{f}.rb", "#{f}.so"]
  if f.start_with?("/", "./", "../")
    cands.each { |p| return [p.end_with?(".so") ? :so : :rb, File.expand_path(p)] if File.file?(p) }
    return nil
  end
  each do |dir|
    cands.each do |p|
      full = File.join(dir.to_s, p)
      return [p.end_with?(".so") ? :so : :rb, File.expand_path(full)] if File.file?(full)
    end
  end
  nil
end
