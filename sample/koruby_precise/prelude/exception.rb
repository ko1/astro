# Exception message helpers.
class Exception
  def detailed_message(highlight: false, **)
    m = message.to_s; cls = self.class; cn = cls.name
    if m.empty?
      text = cls.equal?(RuntimeError) ? 'unhandled exception' : (cn || cls.to_s)
      return highlight ? "\e[1;4m#{text}\e[m" : text
    end
    return (highlight ? "\e[1m#{m}\e[m" : m) if cn.nil?
    highlight ? "\e[1m#{m} (\e[1;4m#{cn}\e[m\e[1m)\e[m" : "#{m} (#{cn})"
  end
  def full_message(highlight: nil, order: nil)
    hl = highlight.nil? ? false : (highlight ? true : false)
    order = (hl ? :top : :bottom) if order.nil?
    bt = backtrace
    bt = (caller(1) || []) if bt.nil? || bt.empty?
    dm = detailed_message(highlight: hl)
    body = bt[0] ? "#{bt[0]}: #{dm}" : dm
    rest = bt.length > 1 ? bt[1..-1] : []
    if order == :bottom
      lines = [hl ? "\e[1mTraceback\e[m (most recent call last):" : "Traceback (most recent call last):"]
      rest.reverse_each { |l| lines << "\tfrom #{l}" }
      lines << body
    else
      lines = [body]
      rest.each { |l| lines << "\tfrom #{l}" }
    end
    lines.join("\n") + "\n"
  end
  # no arg → self; with a message → a new exception of the same class (CRuby clones + replaces).
  def exception(*args); args.empty? ? self : self.class.new(*args); end
  def self.exception(*args); new(*args); end   # Class-level Exception.exception(msg) == new(msg)
  # Equal iff same object, or same class + message + backtrace.
  def ==(other)
    return true if equal?(other)
    return false unless other.is_a?(Exception)
    self.class == other.class && message == other.message && backtrace == other.backtrace
  end
end
module Warning
  def self.[](category); false; end
  def self.[]=(category, flag); flag; end
  def self.warn(msg, category: nil); $stderr.print(msg) if $stderr; nil; end
end
class Object
  # Lazy: the underlying `meth` runs only when the returned Enumerator is
  # driven, so `obj.to_enum.lazy...first(n)` never over-iterates and a `meth`
  # that raises does so at iteration time, not at to_enum time (matches CRuby).
  def to_enum(meth = :each, *args)
    this = self
    sz = (respond_to?(:size) && args.empty?) ? size : nil    # size-preserving enumerators report the receiver's size
    Enumerator.new(sz) { |y| this.send(meth, *args) { |*vs| y << (vs.size <= 1 ? vs[0] : vs) } }
  end
  def enum_for(meth = :each, *args)
    this = self
    sz = (respond_to?(:size) && args.empty?) ? size : nil
    Enumerator.new(sz) { |y| this.send(meth, *args) { |*vs| y << (vs.size <= 1 ? vs[0] : vs) } }
  end
end
# Errno::* — one SystemCallError subclass per errno the platform defines, built
# from the C table so the raise side (korb_errno_name) and the constants can
# never drift apart.  Each class carries its number as ::Errno, like CRuby.
class SystemCallError < StandardError
  def initialize(msg = nil, errno = nil)
    if msg.is_a?(Integer) && errno.nil?    # SystemCallError.new(errno_number)
      errno = msg
      msg = nil
    end
    @errno = errno || (self.class.const_defined?(:Errno) ? self.class::Errno : nil)
    base = @errno ? __strerror(@errno) : nil
    base = "unknown error" if base.nil? || base.empty?
    super(msg ? "#{base} - #{msg}" : base)
  end
  # The C raise path builds the exception without running #initialize, so fall
  # back to the class's own number.
  def errno
    @errno || (self.class.const_defined?(:Errno) ? self.class::Errno : nil)
  end
end
module Errno
  __errno_table.each do |name, num|
    next if const_defined?(name, false)
    cls = Class.new(SystemCallError)
    cls.const_set(:Errno, num)
    const_set(name, cls)
  end
end

# Exception-detail constructors: NameError/NoMethodError carry #name/#receiver/#args;
# KeyError/FrozenError carry #receiver (+ #key).  Stored in the same @__ ivars the
# dispatch-time raises use, so the existing C getters keep working.
class NameError
  UNSET__ = Object.new
  def initialize(msg = nil, name = nil, receiver: UNSET__)
    super(msg)
    @__name = name
    unless receiver.equal?(UNSET__); @__receiver = receiver; @__has_recv = true; end
  end
end
class NoMethodError
  def initialize(msg = nil, name = nil, args = nil, priv = false, receiver: NameError::UNSET__)
    super(msg, name, receiver: receiver)
    @__args = args
  end
end
class KeyError
  def initialize(msg = nil, receiver: NameError::UNSET__, key: NameError::UNSET__)
    super(msg)
    unless receiver.equal?(NameError::UNSET__); @__receiver = receiver; @__has_recv = true; end
    @__key = key unless key.equal?(NameError::UNSET__)
  end
  def receiver; @__has_recv ? @__receiver : raise(ArgumentError, "no receiver is available"); end
  def key; defined?(@__key) ? @__key : raise(ArgumentError, "no key is available"); end
end
class FrozenError
  def initialize(msg = nil, receiver: NameError::UNSET__)
    super(msg)
    unless receiver.equal?(NameError::UNSET__); @__receiver = receiver; @__has_recv = true; end
  end
  def receiver; @__has_recv ? @__receiver : raise(ArgumentError, "no receiver is available"); end
end

class Object
  # Kernel#!~ — the negation of #=~ (an intrinsic in CRuby; a plain method here).
  def !~(other) = !(self =~ other)

  # `when *pats` support: the parser desugars the splat to
  # pats.__korb_when_splat(subject).  Semantics match CRuby's expansion:
  # each element is tested with #===, left to right.
  def __korb_when_splat(subj)
    arr = is_a?(Array) ? self : (respond_to?(:to_a) ? to_a : [self])
    arr.any? { |pat| pat === subj }
  end
end

# LoadError#path — the feature/path that could not be loaded (the C raise sites
# stash it in @__path, like NameError#name).
class LoadError
  def path
    defined?(@__path) ? @__path : nil
  end
end

# Binding の implicit (numbered / it) parameter API — Ruby 4.0。
# koruby は _1.._9 / it を普通のローカルとして持つので、Binding からは
# ここで振り分ける: 暗黙パラメータは #local_variables には出さず、
# #implicit_parameter_get / #implicit_parameters から見せる。
class Binding
  IMPLICIT_PARAM_NAMES__ = [:it, :_1, :_2, :_3, :_4, :_5, :_6, :_7, :_8, :_9].freeze

  alias_method :__lvars_all, :local_variables
  def local_variables
    __lvars_all.reject { |n| IMPLICIT_PARAM_NAMES__.include?(n) }
  end

  def implicit_parameters
    __lvars_all.select { |n| IMPLICIT_PARAM_NAMES__.include?(n) }.sort
  end

  def implicit_parameter_defined?(name)
    __lvars_all.include?(__implicit_param_name(name))
  end

  def implicit_parameter_get(name)
    n = __implicit_param_name(name)
    unless __lvars_all.include?(n)
      raise NameError, "implicit parameter '#{n}' is not defined for #{inspect}"
    end
    local_variable_get(n)
  end

  private def __implicit_param_name(name)
    n = name.is_a?(String) ? name.to_sym : name
    raise TypeError, "#{name.inspect} is not a symbol nor a string" unless n.is_a?(Symbol)
    raise NameError, "'#{n}' is not an implicit parameter" unless IMPLICIT_PARAM_NAMES__.include?(n)
    n
  end
end

class Binding
  # CRuby: "#<Binding:0x0000...>" (既定の Object#inspect は ivar を持たない
  # Binding では "#<Object>" になってしまう)。
  def inspect
    format("#<Binding:0x%016x>", object_id << 1)
  end
  alias_method :to_s, :inspect
end
