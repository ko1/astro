# frozen_string_literal: true
# Coverage — line and method coverage for files loaded (and, in eval mode,
# Strings evaluated) after Coverage.setup / .start.  parse.c instruments those
# sources with counter nodes when they are compiled, so nothing else pays for it;
# the counters live in the VM (korb_cov_*), read here through __cov_peek.
# Branch coverage is not measured: supported?(:branches) is false and its result
# is an empty Hash.
module Coverage
  @state = :idle      # :idle / :suspended / :running (CRuby's names)
  @mode = nil         # nil: the compatible mode (plain line arrays); else a Hash of modes

  MODES__ = %i[lines oneshot_lines methods eval].freeze
  private_constant :MODES__

  def self.supported?(mode)
    raise TypeError, "wrong argument type #{mode.class} (expected Symbol)" unless mode.is_a?(Symbol)
    MODES__.include?(mode)
  end

  def self.setup(opt = (no_arg = true; nil))
    raise RuntimeError, "coverage measurement is already setup" unless @state == :idle
    if no_arg
      mode = nil
    elsif opt == :all
      mode = { lines: true, branches: true, methods: true, eval: true }
    else
      raise TypeError, "no implicit conversion of #{opt.nil? ? 'nil' : opt.class} into Hash" unless opt.respond_to?(:to_hash)
      h = opt.to_hash
      mode = {}
      %i[lines branches methods eval].each { |k| mode[k] = true if h[k] }
      if h[:oneshot_lines]
        raise RuntimeError, "cannot enable lines and oneshot_lines simultaneously" if mode[:lines]
        mode[:oneshot_lines] = true
      end
    end
    @mode = mode
    __cov_setup(mode ? !!mode[:eval] : false, mode ? !!mode[:methods] : false)
    @state = :suspended
    nil
  end

  def self.resume
    raise RuntimeError, "coverage measurement is not set up yet" if @state == :idle
    @state = :running
    __cov_set_state(2)
    nil
  end

  def self.suspend
    raise RuntimeError, "coverage measurement is not running" unless @state == :running
    @state = :suspended
    __cov_set_state(1)
    nil
  end

  def self.start(*args, **kw)
    if kw.empty?
      setup(*args)
    else
      raise ArgumentError, "wrong number of arguments (given #{args.size + 1}, expected 0..1)" unless args.empty?
      setup(kw)
    end
    resume
  end

  def self.running? = @state == :running
  def self.state = @state

  def self.peek_result
    raise RuntimeError, "coverage measurement is not enabled" if @state == :idle
    out = {}
    __cov_peek.each do |path, lines, meths, first|
      out[path] = __format(path, lines, meths, first)
    end
    out
  end

  # No options: stop and clear.  With options, an unnamed one is false (CRuby).
  def self.result(**opt)
    stop, clear = opt.empty? ? [true, true] : [!!opt[:stop], !!opt[:clear]]
    r = peek_result
    if stop && !clear
      warn "stop implies clear", uplevel: 1
      clear = true
    end
    __cov_clear if clear
    if stop
      @state = :idle
      @mode = nil
      __cov_set_state(0)
    end
    r
  end

  # The executable lines of `file` (0) and the others (nil), without running it.
  def self.line_stub(file)
    src = File.read(file)
    n = src.count("\n") + (src.end_with?("\n") || src.empty? ? 0 : 1)
    Array.new(n)
  end

  class << self
    private

    def __format(path, lines, meths, first)
      return lines unless @mode
      h = {}
      h[:lines] = lines if @mode[:lines]
      h[:oneshot_lines] = first if @mode[:oneshot_lines]   # in the order they first ran
      h[:branches] = {} if @mode[:branches]
      if @mode[:methods]
        m = {}
        meths.each do |name, sl, sc, el, ec, count|
          klass = __owner_of(path, name, sl)
          m[[klass, name, sl, sc, el, ec]] = count if klass
        end
        h[:methods] = m
      end
      h
    end

    # The module a `def` at path:line defined `name` in (CRuby keys on it).
    def __owner_of(path, name, line)
      ObjectSpace.each_object(Module) do |mod|
        next unless mod.method_defined?(name, false) || mod.private_method_defined?(name, false)
        return mod if mod.instance_method(name).source_location == [path, line]
      end
      nil
    end
  end
end
