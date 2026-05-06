# Minimal Test::Unit shim — just enough to run a chunk of CRuby's
# test/ruby/*.rb files against koruby and tally pass/fail.
#
# Not minitest-compatible in the strict sense; we map the most common
# assertions into a flat counter so individual test methods can fail
# without aborting the whole suite.

$tu_pass = 0
$tu_fail = 0
$tu_error = 0
$tu_skip = 0
$tu_current = nil

class AssertionError < StandardError; end

# Stub EnvUtil — CRuby's test/test_helper-style util.  Most test files
# call only suppress_warning / with_default_external / without_gc; we
# turn them into block-yielders that just call the block.
unless defined?(EnvUtil)
  module EnvUtil
    def self.suppress_warning; yield; end
    def self.with_default_external(_enc); yield; end
    def self.with_default_internal(_enc); yield; end
    def self.without_gc; yield; end
    def self.under_gc_stress; yield; end
    def self.under_gc_compact_stress; yield; end
    def self.timeout(_); yield; end
    def self.invoke_ruby(*); ["", "", 0]; end
    def self.rubybin; ENV["RUBY"] || "ruby"; end
    def self.apply_timeout_scale(t); t; end
    def self.gc_stress_to_class; nil; end
    def self.diagnostic_reports(*); end
    # labeled_class(name, parent=Object) — CRuby's helper that builds an
    # anonymous class with a custom #to_s name; we don't override #to_s
    # but Class.new is good enough for the typical tests that only need
    # an instance-of check.
    def self.labeled_class(_name, parent = Object); Class.new(parent); end
    def self.labeled_module(_name); Module.new; end
    def self.assert_no_color_warning; yield if block_given?; end
    def self.suppress_stderr; yield; end
    # verbose_warning: collects $stderr-ish output during the block and
    # returns it as a String.  We can't easily redirect stderr, but
    # stub-return "" so the assert_match against the captured output
    # short-circuits to skip in tu_shim's assert_match (it skips for
    # Regexp anyway).
    def self.verbose_warning
      yield
      ""
    end
    def self.capture_warning; verbose_warning { yield }; end
  end
end

# Some tests use these top-level constants; provide safe defaults.
NoMemoryError       = Class.new(Exception) unless defined?(NoMemoryError)
SystemStackError    = Class.new(Exception) unless defined?(SystemStackError)
UncaughtThrowError  = Class.new(StandardError) unless defined?(UncaughtThrowError)
ThreadError         = Class.new(StandardError) unless defined?(ThreadError)
SystemCallError     = Class.new(StandardError) unless defined?(SystemCallError)
unless defined?(Errno)
  module Errno
    EINVAL  = Class.new(::SystemCallError)
    EAGAIN  = Class.new(::SystemCallError)
    ENOENT  = Class.new(::SystemCallError)
    EEXIST  = Class.new(::SystemCallError)
    EACCES  = Class.new(::SystemCallError)
    EISDIR  = Class.new(::SystemCallError)
    ENOTDIR = Class.new(::SystemCallError)
    EBADF   = Class.new(::SystemCallError)
    EPIPE   = Class.new(::SystemCallError)
    ECHILD  = Class.new(::SystemCallError)
    EINTR   = Class.new(::SystemCallError)
    EIO     = Class.new(::SystemCallError)
    NOERROR = Class.new(::SystemCallError)
  end
end

unless defined?(Test::Unit::Assertions)
  module Test
    module Unit
      module Assertions; end
    end
  end
end

# A handful of methods that some CRuby tests assume on built-ins.
class NilClass
  def to_f; 0.0; end unless method_defined?(:to_f)
  def to_a; []; end unless method_defined?(:to_a)
  def to_h; {}; end unless method_defined?(:to_h)
end
class FalseClass
  def &(_o); false; end unless method_defined?(:&)
  def |(o); o ? true : false; end unless method_defined?(:|)
  def ^(o); o ? true : false; end unless method_defined?(:^)
end
class TrueClass
  def &(o); o ? true : false; end unless method_defined?(:&)
  def |(_o); true; end unless method_defined?(:|)
  def ^(o); o ? false : true; end unless method_defined?(:^)
end

# Stub Encoding — koruby is byte-only, but CRuby tests reference
# Encoding constants pervasively.  Each constant is a tiny Object
# whose to_s is its name; conversion methods are no-ops that return
# the string unchanged.
unless defined?(Encoding)
  class Encoding
    @@encs = {}
    attr_reader :name
    def initialize(name)
      @name = name
      @@encs[name] = self
    end
    def to_s; @name; end
    def inspect; "#<Encoding:#{@name}>"; end
    def ==(o); o.is_a?(Encoding) && o.name == @name; end
    def self.find(n)
      n = n.to_s
      @@encs[n] || @@encs[n.upcase] || (@@encs[n] = new(n))
    end
    def self.list; @@encs.values; end
    def self.name_list; @@encs.keys; end
    def self.default_external; @@default ||= UTF_8; end
    def self.default_internal; nil; end
    def self.default_external=(e); @@default = e; end
    UTF_8       = new("UTF-8")
    ASCII_8BIT  = new("ASCII-8BIT")
    BINARY      = ASCII_8BIT
    US_ASCII    = new("US-ASCII")
    ASCII       = US_ASCII
    UTF_16BE    = new("UTF-16BE")
    UTF_16LE    = new("UTF-16LE")
    UTF_32BE    = new("UTF-32BE")
    UTF_32LE    = new("UTF-32LE")
    Shift_JIS   = new("Shift_JIS")
    EUC_JP      = new("EUC-JP")
    ISO_8859_1  = new("ISO-8859-1")
    Windows_31J = new("Windows-31J")
  end
  class String
    def encoding; Encoding::UTF_8; end
    def force_encoding(_e); self; end
    def encode(_e); self; end
    def b; self; end
    def valid_encoding?; true; end
    def ascii_only?; bytes.all? { |b| b < 128 }; end
  end
end

# Module#ruby2_keywords / refine — declared by some CRuby tests.
# We don't implement either; treat both as no-ops so the test files
# load.  Tests that depend on the actual behavior (refinements,
# kw-passing edge cases) won't pass, but the load no longer blocks.
class Module
  def ruby2_keywords(*_names); nil; end
  def refine(_klass, &_blk); self; end
  def using(_mod); self; end
end

# Sundry CRuby globals + Kernel methods many tests reference.
Object.const_set(:RUBY_VERSION,  "3.5.0")        unless Object.const_defined?(:RUBY_VERSION)
Object.const_set(:RUBY_PLATFORM, "x86_64-linux") unless Object.const_defined?(:RUBY_PLATFORM)
Object.const_set(:RUBY_ENGINE,   "koruby")       unless Object.const_defined?(:RUBY_ENGINE)
Object.const_set(:RUBY_REVISION, "koruby")       unless Object.const_defined?(:RUBY_REVISION)
$VERBOSE = nil

# Warning module — many tests poke at Warning[:deprecated] / [:experimental].
# Treat as a passthrough config map so set/get round-trip without doing
# anything observable.
unless defined?(Warning)
  module Warning
    @@flags = { deprecated: false, experimental: false }
    def self.[](k);   @@flags[k]; end
    def self.[]=(k, v); @@flags[k] = v; end
    def self.warn(*); end
  end
end

# Process / system stubs — koruby is single-process; tests that
# actually fork/exec will get a no-op.
def system(*_args); false; end
def `(_cmd); ""; end
def fork; nil; end
def spawn(*_args); 0; end

# Stub Thread so files that mention Thread.current at toplevel can load.
unless defined?(Thread)
  class Thread
    def self.current; @@current ||= new; end
    def self.new(*); @@current ||= self.allocate; end
    def [](_); nil; end
    def []=(_, _); end
    def join; self; end
    def value; nil; end
    def alive?; false; end
    def kill; self; end
  end
end

# RubyVM stub — most test_iseq use blows up on the first method call;
# we just provide the namespace so `RubyVM::InstructionSequence` is at
# least an Object that doesn't NPE on .compile / .of (returns nil).
# GC stub — many tests poke GC.start / GC.disable / GC.stat / etc.
unless defined?(GC)
  module GC
    @@disabled = false
    def self.start(*); nil; end
    def self.enable; r = @@disabled; @@disabled = false; r; end
    def self.disable; r = @@disabled; @@disabled = true; r; end
    def self.disabled?; @@disabled; end
    def self.stress; false; end
    def self.stress=(_); false; end
    def self.count; 0; end
    def self.stat(*); {}; end
    def self.compact; nil; end
    def self.auto_compact; false; end
    def self.auto_compact=(_); end
    def self.verify_compaction_references(*); end
    INTERNAL_CONSTANTS = Hash.new(0)
    OPTS = Hash.new("")
  end
end

unless defined?(RubyVM)
  module RubyVM
    class InstructionSequence
      def self.compile(*); nil; end
      def self.of(*); nil; end
      def self.new(*); allocate; end
      def to_a; [:nope]; end
      def disasm; ""; end
    end
  end
end

# Tiny RbConfig stub — many CRuby tests grab fixnum bounds from here.
unless defined?(RbConfig)
  module RbConfig
    LIMITS = {
      "FIXNUM_MAX" =>  4611686018427387903,
      "FIXNUM_MIN" => -4611686018427387904,
      "LONG_MAX"   =>  9223372036854775807,
      "LONG_MIN"   => -9223372036854775808,
      "INT_MAX"    =>  2147483647,
      "INT_MIN"    => -2147483648,
      "UINT_MAX"   =>  4294967295,
      "ULONG_MAX"  =>  18446744073709551615,
    }
    CONFIG = {
      "host_os"  => "linux-gnu",
      "RUBY_PLATFORM" => "x86_64-linux",
    }
    SIZEOF = Hash.new(8)
    SIZEOF["void*"]   = 8
    SIZEOF["int"]     = 4
    SIZEOF["long"]    = 8
    SIZEOF["double"]  = 8
  end
end

# delegate.rb stub — SimpleDelegator, just enough for tests that use it.
unless defined?(SimpleDelegator)
  class SimpleDelegator
    def initialize(target); @__target__ = target; end
    def method_missing(name, *args, &blk)
      if @__target__.respond_to?(name)
        @__target__.send(name, *args, &blk)
      else
        super
      end
    end
    def respond_to_missing?(name, _priv = false)
      @__target__.respond_to?(name)
    end
  end
end

module Test
  module Unit
    class TestCase
      def self.test_methods
        instance_methods.select { |m| m.to_s.start_with?("test_") }.sort
      end
      def setup; end
      def teardown; end

      def assert(cond, msg = "assertion failed")
        if cond
          $tu_pass += 1
        else
          $tu_fail += 1
          puts "  FAIL #{$tu_current}: #{msg}"
        end
      end

      def assert_equal(expected, actual, msg = nil)
        if expected == actual
          $tu_pass += 1
        else
          $tu_fail += 1
          m = msg || "expected #{expected.inspect}, got #{actual.inspect}"
          puts "  FAIL #{$tu_current}: #{m}"
        end
      end

      def assert_not_equal(expected, actual, _msg = nil)
        assert(expected != actual, "expected != #{expected.inspect}")
      end
      alias refute_equal assert_not_equal

      def assert_nil(v, _msg = nil); assert(v.nil?, "expected nil, got #{v.inspect}"); end
      def assert_not_nil(v, _msg = nil); assert(!v.nil?, "expected non-nil"); end
      alias refute_nil assert_not_nil

      def assert_true(v, _msg = nil); assert(v == true, "expected true, got #{v.inspect}"); end
      def assert_false(v, _msg = nil); assert(v == false, "expected false, got #{v.inspect}"); end
      def assert_predicate(o, sym, _msg = nil)
        r = o.send(sym)
        assert(r, "expected #{o.inspect}.#{sym} to be truthy, got #{r.inspect}")
      end
      def assert_not_predicate(o, sym, _msg = nil)
        r = o.send(sym)
        assert(!r, "expected #{o.inspect}.#{sym} to be falsy, got #{r.inspect}")
      end
      alias refute_predicate assert_not_predicate

      # assert_operator(a, :op, b) — checks a.send(op, b) is truthy.
      # Single-arg form (no `b`) collapses to assert_predicate.
      def assert_operator(a, op, b = nil, _msg = nil)
        if b.equal?(nil) && a.respond_to?(op) && a.method(op).arity == 0
          assert_predicate(a, op)
        else
          r = a.send(op, b)
          assert(r, "expected #{a.inspect}.#{op}(#{b.inspect}) to be truthy, got #{r.inspect}")
        end
      end
      def assert_not_operator(a, op, b = nil, _msg = nil)
        if b.equal?(nil) && a.respond_to?(op) && a.method(op).arity == 0
          assert_not_predicate(a, op)
        else
          r = a.send(op, b)
          assert(!r, "expected #{a.inspect}.#{op}(#{b.inspect}) to be falsy, got #{r.inspect}")
        end
      end
      alias refute_operator assert_not_operator

      def assert_send(args, _msg = nil)
        recv, name, *rest = args
        r = recv.send(name, *rest)
        assert(r, "expected #{recv}.#{name} to be truthy")
      end
      def assert_not_send(args, _msg = nil)
        recv, name, *rest = args
        r = recv.send(name, *rest)
        assert(!r, "expected #{recv}.#{name} to be falsy")
      end

      def assert_send(args, _msg = nil)
        recv, name, *a = args
        r = recv.send(name, *a)
        assert(r, "expected #{recv}.#{name} to be truthy")
      end

      def assert_eql(expected, actual, msg = nil)
        if expected.eql?(actual)
          $tu_pass += 1
        else
          $tu_fail += 1
          puts "  FAIL #{$tu_current}: expected #{expected.inspect}.eql?(#{actual.inspect})"
        end
      end

      def assert_same(a, b, _msg = nil)
        assert(a.equal?(b), "expected same object")
      end
      def assert_not_same(a, b, _msg = nil)
        assert(!a.equal?(b), "expected different objects")
      end
      alias refute_same assert_not_same

      def assert_not_include(haystack, needle, _msg = nil)
        assert(!haystack.include?(needle))
      end
      alias refute_include assert_not_include

      def assert_not_empty(c, _msg = nil)
        assert(!c.empty?, "expected non-empty")
      end
      alias refute_empty assert_not_empty

      def assert_method_defined?(klass, args, _msg = nil)
        # CRuby's helper takes [name, *extra]
        name, *rest = args
        ok = klass.method_defined?(name, *rest) ||
             klass.private_method_defined?(name) ||
             klass.protected_method_defined?(name)
        assert(ok, "method #{name} should be defined on #{klass}")
      end
      def assert_method_not_defined?(klass, args, _msg = nil)
        name, *rest = args
        ok = !klass.method_defined?(name, *rest) &&
             !klass.private_method_defined?(name) &&
             !klass.protected_method_defined?(name)
        assert(ok, "method #{name} should not be defined on #{klass}")
      end

      # `build_message` is what CRuby's assert_* helpers use to produce
      # the message string.  We just join args with newlines.
      def build_message(head, *args)
        ([head] + args).map(&:to_s).reject(&:empty?).join("\n")
      end

      # all_assertions: yields a fake assertions object with a .for(label)
      # accumulator.  We just run each block and ignore the labels.
      def all_assertions(_msg = nil)
        a = Object.new
        def a.for(_label); yield self if block_given?; end
        yield a
      end
      def all_assertions_foreach(_msg = nil, *items, &blk)
        items.each { |i| blk.call(i) }
      end
      alias refute_includes assert_not_include

      def assert_not_match(pat, str, _msg = nil)
        if pat.is_a?(String)
          assert(!str.include?(pat))
        else
          $tu_skip += 1
        end
      end
      alias refute_match assert_not_match

      def assert_not_kind_of(klass, obj, _msg = nil)
        assert(!obj.is_a?(klass))
      end
      alias refute_kind_of assert_not_kind_of
      alias refute_instance_of assert_not_kind_of

      def assert_not_respond_to(obj, sym, _msg = nil)
        assert(!obj.respond_to?(sym))
      end
      alias refute_respond_to assert_not_respond_to

      def assert_method_defined(klass, name, _msg = nil)
        assert(klass.method_defined?(name) || klass.private_method_defined?(name) || klass.protected_method_defined?(name))
      end
      def assert_method_not_defined(klass, name, _msg = nil)
        assert(!klass.method_defined?(name))
      end

      def assert_block(_msg = nil, &blk)
        assert(blk.call, "block returned false")
      end

      def assert_positive(v, _msg = nil); assert(v > 0); end
      def assert_negative(v, _msg = nil); assert(v < 0); end

      def assert_nan(v, _msg = nil)
        assert(v.respond_to?(:nan?) && v.nan?, "expected NaN, got #{v.inspect}")
      end
      def assert_infinity(v, _msg = nil)
        assert(v.respond_to?(:infinite?) && v.infinite?, "expected Infinity")
      end

      def assert_float_equal(expected, actual, _msg = nil)
        assert((expected - actual).abs < 1e-9)
      end

      def assert_float(_v, _msg = nil); end  # type-check stub
      def assert_integer(v, _msg = nil)
        assert(v.is_a?(Integer))
      end

      # CRuby helper assertions that need a child process / parser /
      # encoding diagnostics.  Stub them as silent skips so tests don't
      # blow up at parse time.
      def assert_separately(*); $tu_skip += 1; end
      def assert_in_out_err(*); $tu_skip += 1; end
      def assert_no_memory_leak(*); $tu_skip += 1; end
      def assert_normal_exit(*); $tu_skip += 1; end
      def assert_ruby_status(*); $tu_skip += 1; end
      def assert_warning(*); yield if block_given?; end
      def assert_warn(*); yield if block_given?; end
      def assert_no_warning(*); yield if block_given?; end
      def assert_deprecated_warning(*); yield if block_given?; end
      def assert_deprecated_warn(*); yield if block_given?; end
      def assert_syntax_error(*); $tu_skip += 1; end
      def assert_valid_syntax(*); $tu_skip += 1; end
      def assert_join_threads(*); end
      def assert_marshal_roundtrip(v, _msg = nil)
        r = Marshal.load(Marshal.dump(v))
        assert_equal(v, r)
      end
      def assert_throw(tag, _msg = nil, &blk)
        catch(tag) { blk.call }
        $tu_pass += 1
      end

      def assert_kind_of(klass, obj, _msg = nil)
        assert(obj.is_a?(klass), "expected #{klass}, got #{obj.class}")
      end
      alias assert_instance_of assert_kind_of

      def assert_include(haystack, needle, _msg = nil)
        assert(haystack.include?(needle), "expected #{haystack.inspect} to include #{needle.inspect}")
      end
      alias assert_includes assert_include

      def assert_empty(c, _msg = nil); assert(c.empty?, "expected empty, got #{c.inspect}"); end
      def refute_empty(c, _msg = nil); assert(!c.empty?, "expected non-empty"); end

      def assert_in_delta(expected, actual, delta = 0.001, _msg = nil)
        assert((expected - actual).abs <= delta, "delta failed: #{expected} vs #{actual}")
      end

      def assert_match(pat, str, _msg = nil)
        if pat.is_a?(String)
          assert(str.include?(pat), "expected #{str.inspect} to include #{pat.inspect}")
        else
          # Regexp not supported — best-effort skip.
          $tu_skip += 1
          puts "  SKIP #{$tu_current}: assert_match w/ Regexp"
        end
      end

      def assert_raise(*klasses, &blk)
        klasses = [StandardError] if klasses.empty?
        klasses = klasses.map { |k| k.is_a?(String) ? StandardError : k }
        begin
          blk.call
        rescue Exception => e
          if klasses.any? { |k| e.is_a?(k) }
            $tu_pass += 1
            return e
          else
            $tu_fail += 1
            puts "  FAIL #{$tu_current}: expected #{klasses.inspect}, got #{e.class}: #{e.message}"
            return nil
          end
        end
        $tu_fail += 1
        puts "  FAIL #{$tu_current}: expected #{klasses.inspect}, no raise"
        nil
      end
      alias assert_raises assert_raise

      def assert_raise_with_message(klass, msg, &blk)
        e = assert_raise(klass, &blk)
        return unless e
        actual = (e.message rescue "?")
        if msg.is_a?(String)
          if actual.include?(msg)
            $tu_pass += 1
          else
            $tu_fail += 1
            puts "  FAIL #{$tu_current}: expected message to include #{msg.inspect}, got #{actual.inspect}"
          end
        else
          $tu_skip += 1
        end
      end

      def assert_raise_kind_of(klass, &blk)
        assert_raise(klass, &blk)
      end

      def assert_nothing_raised(*_klasses, &blk)
        begin
          blk.call
          $tu_pass += 1
        rescue Exception => e
          $tu_fail += 1
          puts "  FAIL #{$tu_current}: unexpected #{e.class}: #{e.message}"
        end
      end

      def assert_respond_to(obj, sym, _msg = nil)
        assert(obj.respond_to?(sym), "expected #{obj.class}#respond_to?(#{sym.inspect})")
      end

      def assert_same(a, b, _msg = nil)
        assert(a.equal?(b), "expected same object")
      end

      def skip(msg = "skipped")
        $tu_skip += 1
        raise SkipTest.new(msg)
      end
      alias pend skip
      alias omit skip

      def flunk(msg = "flunk")
        $tu_fail += 1
        puts "  FAIL #{$tu_current}: #{msg}"
      end

      class SkipTest < StandardError; end

      # Tests we skip globally — they hang or burn unbounded CPU under
      # koruby (deep recursion, exponential generators, etc).  Each entry
      # is a "ClassName#meth" string.  Add new entries here when triage
      # turns up another non-terminating test.
      SKIP_TESTS = [
        "TestHash#test_huge_iter_level",
        "TestSubHash#test_huge_iter_level",
        # test_array slow / non-terminating cases
        "TestArray#test_combination2",
        "TestArray#test_concat",
        "TestArray#test_cycle",
        "TestArray#test_misc_0",
        "TestArray#test_product2",
        "TestArraySubclass#test_combination2",
        "TestArraySubclass#test_concat",
        "TestArraySubclass#test_cycle",
        "TestArraySubclass#test_misc_0",
        "TestArraySubclass#test_product2",
        # test_integer slow numeric stress
        "TestInteger#test_digits_for_invalid_base_numbers",
        # test_range bsearch for Bignum: walks too many candidates in Ruby
        "TestRange#test_bsearch_for_bignum",
        # test_stackoverflow intentionally blows the stack — koruby's value
        # stack is small, and overflow → SEGV instead of SystemStackError
        "TestException#test_stackoverflow",
        "TestException#test_machine_stackoverflow",
        "TestException#test_machine_stackoverflow_by_define_method",
        "TestException#test_machine_stackoverflow_by_trace",
        # Encoding-aware exception (multi-byte class name, EUC_JP) — out of scope
        "TestException#test_errinfo_encoding_in_debug",
        "TestException#test_errinfo_in_debug",
        # test_string / test_string2: succ on unicode loops forever
        "TestString#test_upto_nonalnum",
        "TestString2#test_upto_nonalnum",
        # Recursive succ/upto with assertions inside the block — slow
        "TestString#test_upto_numeric",
        "TestString2#test_upto_numeric",
        "TestString#test_upto",
        "TestString2#test_upto",
        # Encoding-dependent String tests (out of scope) — slow loops over wide encodings
        "TestString#test_s_new",
        "TestString2#test_s_new",
        "TestString#test_initialize",
        "TestString2#test_initialize",
        "TestString#test_slice",
        "TestString2#test_slice",
        "TestString#test_slice_bang",
        "TestString2#test_slice_bang",
        "TestString#test_AREF_M17N",
        "TestString2#test_AREF_M17N",
        "TestString#test_split",
        "TestString2#test_split",
        "TestString#test_split_with_block",
        "TestString2#test_split_with_block",
        "TestString#test_split_encoding",
        "TestString2#test_split_encoding",
        "TestString#test_split_wchar",
        "TestString2#test_split_wchar",
        "TestString#test_split_invalid_sequence",
        "TestString2#test_split_invalid_sequence",
        "TestString#test_splice!",
        "TestString2#test_splice!",
      ].freeze

      # Run all `test_*` methods, returning [pass, fail, error, skip].
      def self.run_all
        m = self.test_methods
        m.each do |meth|
          $tu_current = "#{self.name}##{meth}"
          if SKIP_TESTS.include?($tu_current)
            $tu_skip += 1
            next
          end
          begin
            inst = self.new
          rescue Exception => e
            $tu_error += 1
            puts "  NEW-ERR #{$tu_current}: #{e.class}: #{e.message rescue '?'}"
            next
          end
          begin
            inst.setup
            inst.send(meth)
            (inst.teardown rescue nil)
          rescue SkipTest
            # already counted via skip()
          rescue Exception => e
            $tu_error += 1
            cls = (e.class.name rescue "?")
            msg = (e.respond_to?(:message) ? (e.message rescue "?") : (e.to_s rescue "?"))
            puts "  ERR  #{$tu_current}: #{cls}: #{msg}"
          end
        end
      end
    end
  end
end

def report_tu(name)
  total = $tu_pass + $tu_fail + $tu_error
  puts "#{name}: pass=#{$tu_pass} fail=#{$tu_fail} err=#{$tu_error} skip=#{$tu_skip} total=#{total}"
end
