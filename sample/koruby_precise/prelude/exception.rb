# Exception message helpers.
class Exception
  # true when stderr is a terminal — the default for :highlight.
  def self.to_tty?
    $stderr.tty?
  rescue StandardError
    false
  end

  def detailed_message(highlight: false, **)
    unless highlight == true || highlight == false
      raise ArgumentError, "expected true or false as highlight: #{highlight.inspect}"
    end
    m = message.to_s; cls = self.class; cn = cls.name
    if m.empty?
      text = cls.equal?(RuntimeError) ? 'unhandled exception' : (cn || cls.to_s)
      return highlight ? "\e[1;4m#{text}\e[m" : text
    end
    # the class name goes at the end of the FIRST line; later lines are
    # decorated on their own (CRuby).
    lines = m.split("\n", -1)
    lines.pop if lines.length > 1 && lines[-1].empty?
    first = lines[0]
    head = if cn.nil?
             highlight ? "\e[1m#{first}\e[m" : first
           elsif highlight
             "\e[1m#{first} (\e[1;4m#{cn}\e[m\e[1m)\e[m"
           else
             "#{first} (#{cn})"
           end
    return head if lines.length == 1
    ([head] + lines[1..-1].map { |l| highlight ? "\e[1m#{l}\e[m" : l }).join("\n")
  end

  def full_message(highlight: nil, order: nil, **opts)
    unless highlight.nil? || highlight == true || highlight == false
      raise ArgumentError, "expected true or false as highlight: #{highlight.inspect}"
    end
    unless order.nil? || order == :top || order == :bottom
      raise ArgumentError, "expected :top or :bottom as order: #{order.inspect}"
    end
    hl = highlight.nil? ? Exception.to_tty? : highlight
    order = :top if order.nil?
    dopts = opts.merge(highlight: hl)
    chunk = lambda do |exc|
      bt = exc.backtrace
      bt = (caller(1) || []) if bt.nil? || bt.empty?
      dm = exc.respond_to?(:detailed_message) ? exc.detailed_message(**dopts) : nil
      dm = dm.to_str if !dm.nil? && !dm.is_a?(String) && dm.respond_to?(:to_str)
      if dm.nil? || !dm.is_a?(String)          # nil / unusable → just the class name
        cn = exc.class.name || exc.class.to_s
        dm = hl ? "\e[1;4m#{cn}\e[m" : cn
      end
      body = bt[0] ? "#{bt[0]}: #{dm}" : dm
      rest = bt.length > 1 ? bt[1..-1] : []
      [body, rest]
    end
    # cause chain: self が最初、cause を辿って古いものへ (CRuby order)
    chain = [self]
    seen = {self.object_id => true}
    e = self
    while (cs = e.cause) && !seen[cs.object_id]
      chain << cs; seen[cs.object_id] = true; e = cs
    end
    sections = chain.map { |exc| chunk.call(exc) }
    if order == :bottom
      lines = [hl ? "\e[1mTraceback\e[m (most recent call last):" : "Traceback (most recent call last):"]
      sections.reverse_each do |body, rest|
        rest.reverse_each { |l| lines << "\tfrom #{l}" }
        lines << body
      end
    else
      lines = []
      sections.each do |body, rest|
        lines << body
        rest.each { |l| lines << "\tfrom #{l}" }
      end
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
  # The categories CRuby knows, each off by default.  An unknown name is an
  # error rather than a silent false, so a typo does not quietly disable.
  CATEGORIES__ = { deprecated: false, experimental: true, performance: false,
                   strict_unused_block: false }
  def self.__category!(category)
    raise TypeError, "wrong argument type #{category.class} (expected Symbol)" unless category.is_a?(Symbol)
    raise ArgumentError, "unknown category: #{category}" unless CATEGORIES__.key?(category)
    category
  end
  def self.categories; CATEGORIES__.keys.sort; end
  def self.[](category); CATEGORIES__[__category!(category)]; end
  def self.[]=(category, flag); CATEGORIES__[__category!(category)] = flag ? true : false; flag; end
  # #warn is an *instance* method made available on the module by `extend self`,
  # exactly as CRuby has it: that is what makes Warning.method(:warn).owner
  # Warning (not its singleton) and lets a program reopen `module Warning` and
  # call `super`.
  # A categorised warning is suppressed when its category flag is off (CRuby).
  def warn(msg, category: nil)
    return nil if category && CATEGORIES__.key?(category) && !CATEGORIES__[category]
    $stderr.print(msg) if $stderr
    nil
  end
  extend self
end

module Kernel
  # Kernel#warn builds one String from its arguments and hands it to
  # Warning.warn, which is the hook programs override.  `category:` is only
  # forwarded when the (possibly redefined) Warning.warn accepts keywords, and
  # the delegation is skipped when self *is* Warning so a `super` inside a
  # redefined Warning.warn cannot recurse.
  private def warn(*msgs, uplevel: nil, category: nil)
    return nil if $VERBOSE.nil?
    unless uplevel.nil?
      unless uplevel.is_a?(Integer)
        raise TypeError, "no implicit conversion of #{uplevel.nil? ? 'nil' : uplevel.class} into Integer" unless uplevel.respond_to?(:to_int)
        uplevel = uplevel.to_int
        raise TypeError, "can't convert to Integer" unless uplevel.is_a?(Integer)
      end
      raise ArgumentError, "negative level (#{uplevel})" if uplevel < 0
    end
    unless category.nil?
      raise TypeError, "no implicit conversion of #{category.class} into Symbol" unless category.respond_to?(:to_sym)
      category = category.to_sym
      # the category gate lives here (CRuby): a disabled category never reaches
      # Warning.warn at all
      return nil unless Warning[category]
    end
    return nil if msgs.empty?
    msg = +""
    append = lambda do |m|                    # an Array argument prints one element per line (like puts)
      if m.is_a?(Array)
        m.each { |e| append.call(e) }
      else
        s = m.to_s
        msg << s
        msg << "\n" unless s.end_with?("\n")
      end
    end
    msgs.each { |m| append.call(m) }
    return ($stderr.write(msg) if $stderr) && nil if equal?(Warning)
    # CRuby's rule verbatim: a Warning.warn of arity 1 takes the message alone;
    # anything else is handed the category keyword too.
    if Warning.method(:warn).arity != 1
      Warning.warn(msg, category: category)
    else
      Warning.warn(msg)
    end
    nil
  end
  module_function :warn      # CRuby exposes it both ways: private Kernel#warn and public Kernel.warn
end
class Object
  # CRuby's copy chain: #dup calls #initialize_dup and #clone calls
  # #initialize_clone, and both default to #initialize_copy.
  private def initialize_dup(orig); initialize_copy(orig); self; end
  private def initialize_clone(orig, freeze: nil); initialize_copy(orig); self; end

  # Lazy: the underlying `meth` runs only when the returned Enumerator is
  # driven, so `obj.to_enum.lazy...first(n)` never over-iterates and a `meth`
  # that raises does so at iteration time, not at to_enum time (matches CRuby).
  def to_enum(meth = :each, *args, &sz_block)
    this = self
    # #size is nil unless a size block was given, and that block stays deferred
    # (#size calls it, not to_enum).  CRuby has no size function for a plain
    # to_enum: only the builtins that build their own enumerator know a size.
    # y.yield (not y <<) so the source's `yield` sees what the consumer sent
    # back — that is what Enumerator#feed sets.
    e = Enumerator.new(sz_block) { |y| this.send(meth, *args) { |*vs| y.yield(*vs) } }
    e.__set_source(this, meth, args)                        # Enumerator#each(*extra) re-drives the source
    e
  end
  alias_method :enum_for, :to_enum    # CRuby: the same definition, not a copy

  # What a block-less builtin returns: CRuby's own enumerator for `meth`, whose
  # #size is the receiver's — except for the predicate-driven ones, where how
  # many elements survive is not knowable up front.
  private def __to_enum_sized(meth, *args)
    e = to_enum(meth, *args)
    return e unless respond_to?(:size) && args.empty?
    case meth
    when :find, :detect, :find_index, :take_while, :drop_while then e
    else e.__set_size(size)
    end
  end

  def display(port = $stdout)
    port.write(to_s)
    nil
  end
end
# Errno::* — one SystemCallError subclass per errno the platform defines, built
# from the C table so the raise side (korb_errno_name) and the constants can
# never drift apart.  Each class carries its number as ::Errno, like CRuby.
class SystemCallError < StandardError
  # errno 引数の coercion (Float 切り捨て / 実数 Complex / #to_int)。
  def self.__scerr_num(errno)
    case errno
    when nil then nil
    when Integer then errno
    when Float then errno.to_i
    when Complex
      raise RangeError, "can't convert #{errno} into Integer" unless errno.imaginary == 0
      errno.real.to_i
    else
      unless errno.respond_to?(:to_int)
        raise TypeError, "no implicit conversion of #{errno.class} into Integer"
      end
      errno.to_int
    end
  end
  def self.__scerr_int_p(v)
    v.is_a?(Integer) || v.is_a?(Float) || v.is_a?(Complex) || v.respond_to?(:to_int)
  end
  # SystemCallError.new は errno に対応する Errno::* クラスを返す (CRuby)。
  def self.new(*args)
    if self == SystemCallError
      raise ArgumentError, "wrong number of arguments (given 0, expected 1..3)" if args.empty?
      num = (args[0].is_a?(Integer) && args.size == 1) ? args[0] : __scerr_num(args[1])
      cls = num && Errno::BY_NUM__[num]
      return cls.new(*args) if cls
    end
    super(*args)
  end
  def initialize(*args)
    msg = errno = loc = nil
    if args[0].is_a?(Integer) && args.size == 1     # SystemCallError.new(errno_number)
      errno = args[0]
    elsif self.class.const_defined?(:Errno, false) && !(args.size >= 2 && SystemCallError.__scerr_int_p(args[1]))
      msg, loc = args[0], args[1]                   # Errno::X.new(msg[, location])
      unless msg.nil?
        msg = msg.to_str if !msg.is_a?(String) && msg.respond_to?(:to_str)
        raise TypeError, "no implicit conversion of #{args[0].class} into String" unless msg.is_a?(String)
      end
    else
      msg, errno, loc = args[0], args[1], args[2]
      unless msg.nil?
        msg = msg.to_str if !msg.is_a?(String) && msg.respond_to?(:to_str)
        raise TypeError, "no implicit conversion of #{args[0].class} into String" unless msg.is_a?(String)
      end
      errno = SystemCallError.__scerr_num(errno)
    end
    @errno = errno || (self.class.const_defined?(:Errno, false) ? self.class::Errno : nil)
    base = @errno ? __strerror(@errno) : nil
    base = "unknown error" if base.nil? || base.empty?
    base = "#{base} @ #{loc}" if loc
    super(msg ? "#{base} - #{msg}" : base)
  end
  # The C raise path builds the exception without running #initialize, so fall
  # back to the class's own number.
  def errno
    @errno || (self.class.const_defined?(:Errno, false) ? self.class::Errno : nil)
  end
end
module Errno
  BY_NUM__ = {}
  # 同じ番号の別名 (EAGAIN/EWOULDBLOCK 等) は同じクラス。クラス名は CRuby の
  # 正準名に合わせるため、正準名を先に登録する 2-pass。
  CANON__ = { "EWOULDBLOCK" => "EAGAIN", "EOPNOTSUPP" => "ENOTSUP", "EDEADLOCK" => "EDEADLK" }
  tbl = __errno_table
  [true, false].each do |canon_pass|
    tbl.each do |name, num|
      next if CANON__.key?(name.to_s) == canon_pass   # pass1: 正準名, pass2: 別名
      unless const_defined?(name, false)
        if num > 0 && BY_NUM__[num]
          const_set(name, BY_NUM__[num])
        else
          cls = Class.new(SystemCallError)
          cls.const_set(:Errno, num)
          const_set(name, cls)
        end
      end
      BY_NUM__[num] ||= const_get(name) if num > 0
    end
  end
end

# Exception-detail constructors: NameError/NoMethodError carry #name/#receiver/#args;
# KeyError/FrozenError carry #receiver (+ #key).  Stored in the same @__ ivars the
# dispatch-time raises use, so the existing C getters keep working.
class SyntaxError
  # The file the parser was reading (eval's 3rd argument), or nil.
  def path; defined?(@__path) ? @__path : nil; end
end
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

  def eval(src, *rest)
    Kernel.eval(src, self, *rest)
  end

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

class SystemExit
  # Kernel#exit が積んだ終了ステータス (既定 0)。
  def status; defined?(@__status) ? @__status : 0; end
  def success?; status == 0; end
  def initialize(*args)
    st = args.first
    if st.is_a?(Integer) || st == true || st == false
      @__status = (st == true ? 0 : st == false ? 1 : st)
      super(*args[1..])
    else
      @__status = 0
      super(*args)
    end
  end
end
