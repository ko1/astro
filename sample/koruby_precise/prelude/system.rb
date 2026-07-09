# Minimal stubs for the process/thread/GC constants that mspec (and some specs)
# reference at load time.  koruby is single-threaded and non-forking; these give
# just enough surface to load mspec and run non-concurrent specs.  Real behavior
# is out of scope (Thread.new runs its block synchronously).

class Thread
  def self.current; @current ||= allocate; end
  def self.main; current; end
  def self.list; [current]; end
  def self.pass; nil; end
  def self.stop; nil; end
  def self.new(*args, &block)
    t = allocate
    # Lazily run the block on first #value/#join instead of at creation, so
    # Thread.new { loop {} } (concurrency specs) doesn't hang the whole file.
    t.instance_variable_set(:@__blk, block)
    t.instance_variable_set(:@__args, args)
    t
  end
  def self.start(*a, &b); new(*a, &b); end
  def __run
    return @__value if defined?(@__value) && @__ran
    @__ran = true
    @__value = @__blk ? @__blk.call(*@__args) : nil
  end
  def self.report_on_exception; @roe; end
  def self.report_on_exception=(v); @roe = v; end
  def self.abort_on_exception; @aoe; end
  def self.abort_on_exception=(v); @aoe = v; end
  def value; __run; end
  def join(*); __run; self; end
  def alive?; false; end
  def kill; self; end
  def name; @__name; end
  def name=(n); @__name = n; end
  def [](k); (@__tls ||= {})[k]; end
  def []=(k, v); (@__tls ||= {})[k] = v; end
  def report_on_exception; @roe; end
  def report_on_exception=(v); @roe = v; end
end

class Mutex
  def lock; @locked = true; self; end
  def unlock; @locked = false; self; end
  def locked?; @locked ? true : false; end
  def try_lock; @locked ? false : (@locked = true); end
  def synchronize; lock; begin; yield; ensure; unlock; end; end
  def owned?; @locked ? true : false; end
end
Thread::Mutex = Mutex

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
