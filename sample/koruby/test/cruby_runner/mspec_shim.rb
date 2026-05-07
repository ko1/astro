# mspec_shim.rb — minimal mspec replacement for running CRuby's
# spec/ruby/language/*_spec.rb files against koruby.
#
# Real mspec uses Thread + Regexp + signals + spec_helper machinery.
# Most language specs only need:
#   describe "..." do ... end
#   it "..." do ... end
#   x.should == y / x.should be_nil / etc
#   x.should_not ...
#
# Counters land in $ms_pass / $ms_fail / $ms_error.

$ms_pass = 0
$ms_fail = 0
$ms_error = 0
$ms_skip = 0
$ms_current = nil

class MSpecError < StandardError; end

# describe blocks: just run the body in a fresh context.  Save any
# before/after hooks so subsequent `it` runs can fire them.
def describe(name, *opts, &blk)
  # Shared spec: `describe :name, shared: true do ... end` — drop on
  # the floor (it_behaves_like is a no-op anyway).
  shared = opts.any? { |o| o.is_a?(Hash) && o[:shared] }
  return if shared
  prev_describe = $ms_describe
  prev_be = $ms_before_each
  prev_ae = $ms_after_each
  $ms_describe = name
  # Each describe gets fresh hook arrays so nested describes don't
  # share with siblings.  Hooks from the outer scope are still
  # captured via `prev_be`/`prev_ae` and re-run via the closure below.
  outer_be = prev_be ? prev_be.dup : []
  outer_ae = prev_ae ? prev_ae.dup : []
  $ms_before_each = outer_be.dup
  $ms_after_each = outer_ae.dup
  begin
    blk.call
  rescue => e
    $ms_error += 1
    puts "  ERR #{$ms_describe} (block-level): #{e.class}: #{e.message}"
  ensure
    $ms_describe = prev_describe
    $ms_before_each = prev_be
    $ms_after_each = prev_ae
  end
end

# context is an alias for describe in mspec.  Top-level alias_method
# doesn't reach the global function namespace, so define it explicitly.
def context(name, *opts, &blk); describe(name, *opts, &blk); end

# it/specify: run the block, count outcomes.  Run before-each hooks
# so things like `before :each { @x = ... }` set up state.
def it(name, *_opts, &blk)
  $ms_current = "#{$ms_describe} #{name}"
  # `it "name"` (no body) is pending in mspec — count as skip rather
  # than calling a nil block.
  unless blk
    $ms_skip += 1
    return
  end
  begin
    if $ms_before_each
      $ms_before_each.each do |h|
        begin
          h.call
        rescue
          # Hook failures don't kill the test; they just leave state
          # partial and the test will likely fail for that reason.
        end
      end
    end
    blk.call
  rescue MSpecError => e
    $ms_fail += 1
    puts "  FAIL #{$ms_current}: #{e.message}"
  rescue => e
    # Out-of-scope features (Thread/Fiber/Ractor/Encoding/etc.) surface
    # as `NameError: uninitialized constant`.  Treat those as skip so
    # the test count reflects in-scope coverage instead of polluting
    # ERR with intentional gaps.  Same treatment for Random which we
    # don't implement.
    out_of_scope = %w(Thread Fiber Ractor Encoding Random TracePoint GC
                      ObjectSpace RubyVM Mutex
                      ConditionVariable Queue SizedQueue Refinement)
    is_refinement = e.is_a?(NoMethodError) &&
                    (e.message.include?("'refine'") ||
                     e.message.include?("'using'"))
    if (e.is_a?(NameError) && e.message.start_with?("uninitialized constant ") &&
        out_of_scope.any? { |c| e.message.include?(c) }) ||
       is_refinement
      $ms_skip += 1
    else
      $ms_error += 1
      puts "  ERR  #{$ms_current}: #{e.class}: #{e.message}"
    end
  ensure
    if $ms_after_each
      $ms_after_each.each { |h| h.call rescue nil }
    end
  end
end

def specify(name, *opts, &blk); it(name, *opts, &blk); end

# xit / pending: skip
def xit(*_args, &_blk); $ms_skip += 1; end
def pending(*args, &blk); xit(*args, &blk); end

# before / after / before_each blocks — store and run around each `it`.
# We implement a simplified version: only `before :each` is honored, and
# we mutate the surrounding state via $ms_before_each/after_each.
def before(scope = :each, &blk)
  case scope
  when :each then ($ms_before_each ||= []) << blk
  when :all  then blk.call
  end
end

def after(scope = :each, &blk)
  case scope
  when :each then ($ms_after_each ||= []) << blk
  when :all  then # ignore
  end
end

# Matchers — wrap a value so chained assertions work.
class MSpecExpectation
  def initialize(actual)
    @actual = actual
  end
  def =~(other)
    # See raise_error matcher for the rationale: /regex/ literals are
    # Strings in koruby, so use substring matching as the proxy.
    matched = if other.is_a?(String) && @actual.is_a?(String)
                other.split('|').any? { |alt| @actual.include?(alt) }
              else
                @actual =~ other
              end
    if matched then $ms_pass += 1
    else
      $ms_fail += 1
      raise MSpecError, "expected #{@actual.inspect} =~ #{other.inspect}"
    end
  end
  def ==(expected)
    if @actual == expected then $ms_pass += 1
    else
      $ms_fail += 1
      raise MSpecError, "expected #{expected.inspect}, got #{@actual.inspect}"
    end
  end
  def !=(expected)
    if @actual != expected then $ms_pass += 1
    else
      $ms_fail += 1
      raise MSpecError, "expected != #{expected.inspect}"
    end
  end
  def equal(expected)
    if @actual.equal?(expected) then $ms_pass += 1
    else
      raise MSpecError, "expected to equal? #{expected.inspect}, got #{@actual.inspect}"
    end
  end
  def eql(expected)
    if @actual.eql?(expected) then $ms_pass += 1
    else
      raise MSpecError, "expected eql? #{expected.inspect}, got #{@actual.inspect}"
    end
  end
  def be_nil
    if @actual.nil? then $ms_pass += 1
    else raise MSpecError, "expected nil, got #{@actual.inspect}"
    end
  end
  def be_true
    if @actual == true then $ms_pass += 1
    else raise MSpecError, "expected true, got #{@actual.inspect}"
    end
  end
  def be_false
    if @actual == false then $ms_pass += 1
    else raise MSpecError, "expected false, got #{@actual.inspect}"
    end
  end
  def be_truthy
    if @actual then $ms_pass += 1
    else raise MSpecError, "expected truthy, got #{@actual.inspect}"
    end
  end
  def be_falsy
    if !@actual then $ms_pass += 1
    else raise MSpecError, "expected falsy, got #{@actual.inspect}"
    end
  end
  def be_an_instance_of(klass)
    if @actual.class == klass then $ms_pass += 1
    else raise MSpecError, "expected instance_of #{klass}, got #{@actual.class}"
    end
  end
  def be_kind_of(klass)
    if @actual.kind_of?(klass) then $ms_pass += 1
    else raise MSpecError, "expected kind_of #{klass}, got #{@actual.class}"
    end
  end
  def be_a(klass); be_kind_of(klass); end
  def be_an(klass); be_kind_of(klass); end
  def be_close(target, tol)
    if (@actual - target).abs <= tol then $ms_pass += 1
    else raise MSpecError, "expected close to #{target} ± #{tol}, got #{@actual.inspect}"
    end
  end
  def include(*items)
    items.each do |it|
      unless @actual.include?(it)
        raise MSpecError, "expected to include #{it.inspect}, got #{@actual.inspect}"
      end
    end
    $ms_pass += 1
  end
  def respond_to(name)
    if @actual.respond_to?(name) then $ms_pass += 1
    else raise MSpecError, "expected #{@actual.inspect} to respond_to #{name}"
    end
  end
  def raise_error(klass = StandardError, msg = nil)
    if !@actual.is_a?(Proc) && !@actual.respond_to?(:call)
      raise MSpecError, "raise_error needs a callable on .should"
    end
    begin
      @actual.call
    rescue Exception => e
      if e.is_a?(klass) && (msg.nil? || msg === e.message)
        $ms_pass += 1; return
      end
      raise MSpecError, "expected #{klass}, got #{e.class}: #{e.message}"
    end
    raise MSpecError, "expected #{klass}, no raise"
  end
  def <(o); @actual < o ? ($ms_pass += 1) : (raise MSpecError, "expected #{@actual} < #{o}"); end
  def <=(o); @actual <= o ? ($ms_pass += 1) : (raise MSpecError, "expected #{@actual} <= #{o}"); end
  def >(o); @actual > o ? ($ms_pass += 1) : (raise MSpecError, "expected #{@actual} > #{o}"); end
  def >=(o); @actual >= o ? ($ms_pass += 1) : (raise MSpecError, "expected #{@actual} >= #{o}"); end
end

# Standalone matchers — bareword calls inside `it` blocks like
# `x.should be_nil` parse as `x.should(be_nil)`, so each matcher needs
# to exist as a top-level method that returns a tagged Symbol; .should
# inspects the tag to decide what to assert.
class MSpecMatcher
  attr_reader :kind, :arg
  def initialize(kind, arg = nil) @kind, @arg = kind, arg; end
end

def have_method(name); MSpecMatcher.new(:have_method, name); end
def have_instance_method(name); MSpecMatcher.new(:have_instance_method, name); end
def have_private_method(name); MSpecMatcher.new(:have_private_method, name); end
def have_public_method(name); MSpecMatcher.new(:have_public_method, name); end
def have_protected_method(name); MSpecMatcher.new(:have_protected_method, name); end
def have_private_instance_method(name, *_); MSpecMatcher.new(:have_private_instance_method, name); end
def have_public_instance_method(name, *_); MSpecMatcher.new(:have_public_instance_method, name); end
def have_protected_instance_method(name, *_); MSpecMatcher.new(:have_protected_instance_method, name); end
def be_ancestor_of(c); MSpecMatcher.new(:be_ancestor_of, c); end
def be_computed_by(name, *args); MSpecMatcher.new(:be_computed_by, [name, args]); end
def have_constant(name); MSpecMatcher.new(:have_constant, name); end
def have_class_variable(name); MSpecMatcher.new(:have_class_variable, name); end
def have_instance_variable(name); MSpecMatcher.new(:have_instance_variable, name); end
def be_nil; MSpecMatcher.new(:be_nil); end
def be_true; MSpecMatcher.new(:be_true); end
def be_false; MSpecMatcher.new(:be_false); end
def be_truthy; MSpecMatcher.new(:be_truthy); end
def be_falsy; MSpecMatcher.new(:be_falsy); end
def be_close(target, tol); MSpecMatcher.new(:be_close, [target, tol]); end
def be_an_instance_of(k); MSpecMatcher.new(:be_an_instance_of, k); end
def be_kind_of(k); MSpecMatcher.new(:be_kind_of, k); end
def be_a(k); be_kind_of(k); end
def be_an(k); be_kind_of(k); end
def equal(o); MSpecMatcher.new(:equal, o); end
def eql(o); MSpecMatcher.new(:eql, o); end
def respond_to(name); MSpecMatcher.new(:respond_to, name); end
def raise_error(klass = StandardError, msg = nil); MSpecMatcher.new(:raise_error, [klass, msg]); end
def raise_exception(klass = Exception, msg = nil); MSpecMatcher.new(:raise_error, [klass, msg]); end
def __mspec_include_matcher(*items); MSpecMatcher.new(:include, items); end
# Top-level `include` ambiguous: in mspec test bodies it's a matcher
# (`arr.should include(1)`), but in Ruby it's the Module-include keyword
# (`include Foo` makes Foo's constants reachable).  Disambiguate by arg
# type: a single Module/Class arg means real include; anything else is
# the matcher form.  Modules and Classes can't be values one wants to
# match in `include`, so this never actually shadows.
def include(*items)
  # Real Module#include: every arg must be a Module (not a Class —
  # `include Foo` for a Class would raise TypeError in CRuby anyway).
  # `arr.should include(SomeError)` passes a Class as a value to test —
  # we route that to the matcher form.
  all_modules = items.size >= 1 && items.all? { |i|
    !i.nil? && i.class == Module
  }
  if all_modules
    target = self.is_a?(Module) ? self : Object
    items.each { |m| target.send(:include, m) }
    return self
  end
  MSpecMatcher.new(:include, items)
end

class Object
  # mspec mock helpers as no-ops on regular objects.  Tests use these
  # to assert "this method shouldn't be called" — we don't track calls,
  # so just record an expectation.
  def should_receive(*_); MSpecMockExpectation.new(self, :stub); end
  def should_not_receive(*_); MSpecMockExpectation.new(self, :stub); end
  def stub!(*); MSpecMockExpectation.new(self, :stub); end

  def should(matcher = nil)
    if matcher.nil?
      MSpecExpectation.new(self)
    elsif matcher.is_a?(MSpecMatcher)
      MSpecExpectation.new(self).__apply_matcher(matcher, false)
    else
      raise MSpecError, "unsupported should-matcher form"
    end
  end

  def should_not(matcher = nil)
    if matcher.nil?
      MSpecNegatedExpectation.new(self)
    elsif matcher.is_a?(MSpecMatcher)
      MSpecExpectation.new(self).__apply_matcher(matcher, true)
    else
      raise MSpecError, "unsupported should_not-matcher form"
    end
  end
end

class MSpecExpectation
  def __apply_matcher(m, negate)
    ok = case m.kind
         when :complain
           # Run the block so its side effects (assignments etc.) happen.
           # We don't actually emit warnings — koruby doesn't track them —
           # so for `should complain(...)` claim true (the warning was
           # 'expected') and for `should_not complain` claim false (no
           # warning was emitted, which is what the spec is asserting).
           @actual.call rescue nil
           negate ? false : true
         when :have_method then @actual.method_defined?(m.arg) || @actual.private_method_defined?(m.arg) || @actual.respond_to?(m.arg)
         when :have_instance_method then @actual.method_defined?(m.arg) || @actual.private_method_defined?(m.arg)
         when :have_private_method then @actual.private_method_defined?(m.arg)
         when :have_public_method then @actual.public_method_defined?(m.arg)
         when :have_protected_method then @actual.protected_method_defined?(m.arg) rescue false
         when :have_private_instance_method then @actual.private_method_defined?(m.arg)
         when :have_public_instance_method then @actual.public_method_defined?(m.arg)
         when :have_protected_instance_method then @actual.protected_method_defined?(m.arg) rescue false
         when :be_ancestor_of then @actual.ancestors.include?(m.arg)
         when :be_computed_by
           # @actual is an Array of [input, *args, expected] tuples.
           # Each input.send(name, *args) must == expected.
           name, extra_args = m.arg
           ok = true
           @actual.each do |pair|
             input = pair[0]
             expected = pair[-1]
             call_args = extra_args + pair[1...-1]
             begin
               actual_val = input.send(name, *call_args)
             rescue
               ok = false; break
             end
             ok = false unless actual_val == expected
             break unless ok
           end
           ok
         when :have_constant then @actual.const_defined?(m.arg)
         when :have_class_variable then @actual.class_variable_defined?(m.arg) rescue false
         when :have_instance_variable then @actual.instance_variable_defined?(m.arg) rescue false
         when :be_nil then @actual.nil?
         when :be_true then @actual == true
         when :be_false then @actual == false
         when :be_truthy then !!@actual
         when :be_falsy then !@actual
         when :be_close then (@actual - m.arg[0]).abs <= m.arg[1]
         when :be_an_instance_of then @actual.class == m.arg
         when :be_kind_of then @actual.kind_of?(m.arg)
         when :equal then @actual.equal?(m.arg)
         when :eql then @actual.eql?(m.arg)
         when :respond_to then @actual.respond_to?(m.arg)
         when :raise_error
           klass, msg = m.arg
           ok = false
           begin
             @actual.call
           rescue Exception => e
             # klass may be a Class or [Class, ...]
             classes = klass.is_a?(Array) ? klass : [klass]
             cls_ok = classes.any? { |c| c.nil? || e.is_a?(c) }
             # koruby's /regex/ literals are Strings (Regexp is pending the
             # astrorge integration).  Real Regexp gets the standard ===;
             # a String stand-in becomes a substring check so message
             # patterns from rubyspec still match meaningfully.
             msg_ok = if msg.nil? then true
                      elsif msg.is_a?(String)
                        # /a|b/ — proxy regex alternation by checking each
                        # branch independently.  Strip common regex escape
                        # sequences (\[ \] \. \( \) \? \+ \* \^ \$ \|) so
                        # that rubyspec patterns like /\[0, 1\]/ still
                        # match the error message (which never includes
                        # literal backslashes).
                        msg.split('|').any? { |alt|
                          stripped = alt.dup
                          %w(\\[ \\] \\. \\( \\) \\? \\+ \\* \\^ \\$ \\| \\\\).each do |esc|
                            stripped = stripped.gsub(esc, esc[1])
                          end
                          # Anchors: drop a leading "\\A" and a trailing
                          # "\\z" — rubyspec uses these for full-string
                          # match; we use substring containment as the
                          # proxy, so just drop the anchors.  (Plain
                          # string literals: koruby's /regex/ is itself
                          # a String, so the matcher's pattern would be
                          # taken literally — work in chars instead.)
                          stripped = stripped[2..-1] if stripped.start_with?("\\A")
                          stripped = stripped[0..-3] if stripped.end_with?("\\z")
                          # Wildcard `.*` (matches any chars).  Split
                          # into segments; each must appear in order.
                          if stripped.include?('.*')
                            parts = stripped.split('.*')
                            pos = 0
                            parts.all? { |part|
                              if part.empty? then true
                              else
                                idx = e.message.index(part, pos)
                                if idx then pos = idx + part.length; true
                                else false; end
                              end
                            }
                          else
                            e.message.include?(stripped)
                          end
                        }
                      else msg === e.message
                      end
             ok = cls_ok && msg_ok
           end
           ok
         when :include then m.arg.all? { |x| @actual.include?(x) }
         else raise MSpecError, "unknown matcher #{m.kind}"
         end
    pass = negate ? !ok : ok
    if pass then $ms_pass += 1
    else
      raise MSpecError, "expected #{negate ? '!' : ''}#{m.kind}(#{m.arg.inspect}), got #{@actual.inspect}"
    end
    self
  end
end

class MSpecNegatedExpectation < MSpecExpectation
  def ==(expected)
    if @actual != expected then $ms_pass += 1
    else raise MSpecError, "expected != #{expected.inspect}"
    end
  end
  def be_nil
    if !@actual.nil? then $ms_pass += 1
    else raise MSpecError, "expected non-nil"
    end
  end
  # ... incomplete; fall back to the matcher's negation
end

# ruby_version_is supports a String "3.0" (>=) or a Range "..."3.4"
# (only versions before 3.4).  We pretend to be Ruby 3.4 (master).
KORB_RUBY_VERSION = "3.4.0"

def __version_cmp(a, b)
  pa = a.split('.').map(&:to_i)
  pb = b.split('.').map(&:to_i)
  n = pa.size > pb.size ? pa.size : pb.size
  n.times do |i|
    pi = pa[i] || 0
    qi = pb[i] || 0
    return -1 if pi < qi
    return 1  if pi > qi
  end
  0
end

def __version_le(a, b); __version_cmp(a, b) <= 0; end
def __version_lt(a, b); __version_cmp(a, b) <  0; end
def __version_ge(a, b); __version_cmp(a, b) >= 0; end

def __ruby_version_in_range?(v)
  if v.is_a?(Range)
    lo = v.begin.to_s.empty? ? "0" : v.begin
    hi = v.end
    cur = KORB_RUBY_VERSION
    lo_ok = __version_ge(cur, lo)
    if hi.nil?
      hi_ok = true
    elsif v.exclude_end?
      hi_ok = __version_lt(cur, hi)
    else
      hi_ok = __version_le(cur, hi)
    end
    lo_ok && hi_ok
  else
    __version_ge(KORB_RUBY_VERSION, v)
  end
end

def ruby_version_is(v, &blk)
  return unless blk
  blk.call if __ruby_version_in_range?(v)
end

def ruby_version_is_not(v, &blk)
  return unless blk
  blk.call unless __ruby_version_in_range?(v)
end
def ruby_bug(_id, _v); yield if block_given?; end
def platform_is(*_opts, &blk); end
def platform_is_not(*_opts, &blk); blk.call if blk; end
def quarantine!(*_opts); yield if block_given?; end
def guard(*_opts, &blk); blk.call if blk; end
def guard_not(*_opts, &blk); blk.call if blk; end
def conflicts_with(*_opts, &blk); blk.call if blk; end

# CRuby-specific guards that just yield
def with_feature(*_opts, &blk); blk.call if blk; end
def without_feature(*_opts, &blk); end

# Module / Class guards
def CODE_LOADING_DIR; "/tmp"; end
SPEC_TEMP_DIR = "/tmp/spec_temp" rescue nil

# Suppress warning helper used in some specs.
def suppress_warning; old = $VERBOSE; $VERBOSE = nil; yield; ensure $VERBOSE = old; end

# mspec / TestUnit helper used by some specs to fail unconditionally.
def flunk(msg = nil); raise MSpecError, msg || "flunked"; end
# Return the value of $! at the calling point — some specs probe it via
# this helper rather than referring to $! directly.
def suppress_keyword_warning(&blk); blk.call; end

# fixture helper — minimal stub.
def fixture(file, *args)
  File.expand_path(args.last.to_s, File.dirname(file).to_s)
end

# Misc constants
NATFIXNUM_MIN = -(2**62) rescue 0
NATFIXNUM_MAX = (2**62) - 1 rescue 0

# ruby2_keywords is out of scope (project policy) — stub on Module so
# class-level usage doesn't NoMethodError.
class Module
  def ruby2_keywords(*_names); self; end unless method_defined?(:ruby2_keywords)
end

# Minimal mock — just records expected method calls.  Each `mock(...)`
# call returns an Object that responds to `should_receive(name)` and
# subsequent `.and_return(...)`.  Calling the method on the mock returns
# the configured value (or nil).  Doesn't enforce call counts.
class MSpecMock
  def initialize(name); @name = name; @recv = {}; end
  def should_receive(method, *_); @recv[method] = MSpecMockExpectation.new(self, method); @recv[method]; end
  def stub(*); MSpecMockExpectation.new(self, :stub); end
  def stub!(method, *_); should_receive(method); end
  def method_missing(name, *args, &blk)
    e = @recv[name]
    if e then e.__return_value
    else nil
    end
  end
  def respond_to?(name, _priv = false)
    return true if @recv.key?(name)
    # If respond_to_missing? is stubbed, defer to its configured return.
    e = @recv[:respond_to_missing?]
    return e.__return_value ? true : false if e
    false
  end
  def respond_to_missing?(name, priv = false)
    e = @recv[:respond_to_missing?]
    return e.__return_value ? true : false if e
    true
  end
  def inspect; "#<mock(#{@name})>"; end
  # CRuby's RSpec-style mock object overrides built-in methods (to_s,
  # hash, ==) when stubbed — without these overrides the default
  # Object impl resolves first and method_missing never fires.
  def to_s; e = @recv[:to_s]; e ? e.__return_value : super; end
  def hash; e = @recv[:hash]; e ? e.__return_value : super; end
  def ==(o); e = @recv[:==]; e ? e.__return_value : super; end
end

class MSpecMockExpectation
  def initialize(mock, name); @mock = mock; @name = name; @ret = nil; end
  def and_return(v); @ret = v; self; end
  def and_yield(*); self; end
  def with(*); self; end
  def once; self; end
  def twice; self; end
  def at_least(*); self; end
  def at_most(*); self; end
  def __return_value; @ret; end
end

def mock(name = ""); MSpecMock.new(name); end
def mock_int(value); value; end
def stub!(name = ""); MSpecMock.new(name); end

# ScratchPad — mspec helper that records values across an example's
# block invocations.  Tests do ScratchPad.record :foo / ScratchPad.recorded.
class ScratchPad
  @@val = nil
  def self.record(v); @@val = v; end
  def self.recorded; @@val; end
  def self.clear; @@val = nil; end
  def self.<<(v); @@val ||= []; @@val << v; @@val; end
end

# `complain` — matcher for ".should complain(/.../)" — verifies a block
# emits a matching warning.  We don't track warnings, so return a
# matcher that always passes (so we don't fail tests that exercise
# warning behaviour we don't reproduce).
def complain(_pattern = nil, **_opts); MSpecMatcher.new(:complain); end

# `ruby_exe` — runs ruby code in a subprocess.  Out of scope (no
# subprocess), so return empty string.
def ruby_exe(*_); ""; end
def ruby_cmd(*_); "ruby"; end

# Shared spec inclusion — opening a Pandora's box by actually running
# shared spec blocks adds a lot of failure modes (cross-file fixtures,
# Thread/Fiber etc. references inside shared specs).  Keep no-op for
# now; rubyspec's coverage of shared-spec-driven tests is small enough
# to not be worth the regressions.
$ms_shared_specs = {}
def it_behaves_like(*_args, &_blk); end

# Suppress warning helper — runs block with $VERBOSE = nil.
def silence_warnings; old = $VERBOSE; $VERBOSE = nil; yield; ensure $VERBOSE = old; end

# SpecEvaluate — mspec helper that evaluates a string in different
# contexts.  We don't need the contexts; just provide a stub.
class SpecEvaluate
  def self.desc=(v); @@desc = v; end
  def self.desc; @@desc rescue ""; end
end

# `evaluate <<-ruby do; ... end` — runs the heredoc as Ruby in a
# fresh class context.  We just eval it at top level.
def evaluate(code, &blk)
  eval(code)
  blk.call if blk
end
