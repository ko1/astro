# TracePoint.  The hooks are the interpreter's (builtins/tracepoint.c): __trace_hooks(mask) tells it the events
# the enabled TracePoints want, and it calls TracePoint.__fire for each, outside any hook.  @frame holds the
# event's attributes while the block runs; the readers raise "access from outside" otherwise.
#
# prelude はどの起動でも parse されるので、行数がそのまま起動コストになる。
# reader 群を define_method で畳んであるのはそのため。
class TracePoint
  EVENTS = [:line, :class, :end, :call, :return, :c_call, :c_return,
            :b_call, :b_return, :a_call, :a_return, :thread_begin,
            :thread_end, :fiber_switch, :raise, :rescue, :script_compiled].freeze
  # Proc の iseq は method の :call/:return を持てない (CRuby は iseq を走査
  # して判定する; ここは target の種別で近似する)。
  PROC_EVENTS = (EVENTS - [:call, :return]).freeze
  @__active = []

  def self.__rehook
    m = 0
    @__active.each { |tp| m |= tp.__mask }
    __trace_hooks(m)
  end

  def self.__fire(ev, path, lineno, slf, mid, cid, dcls, bind, val, key, params)
    # the global hooks, then the targeted (CRuby's iseq-local) ones; each the last enabled first
    @__active.reverse_each { |tp| tp.__deliver(ev, path, lineno, slf, mid, cid, dcls, bind, val, key, params) unless tp.__targeted? }
    @__active.reverse_each { |tp| tp.__deliver(ev, path, lineno, slf, mid, cid, dcls, bind, val, key, params) if tp.__targeted? }
    nil
  end

  def self.__add(tp); @__active << tp unless @__active.include?(tp); __rehook; end
  def self.__remove(tp); @__active.delete(tp); __rehook; end

  def initialize(*events, &block)
    events = [:line] if events.empty?
    @events = events.map { |e|
      s = e.is_a?(Symbol) ? e : TracePoint.__sym(e)
      raise ArgumentError, "unknown event: #{s}" unless EVENTS.include?(s)
      s
    }
    raise ArgumentError, "must be called with a block" unless block
    @block = block
    @enabled = false
    @target = @target_line = @frame = @thread = @target_key = nil
    @mask = 0                                     # bit i: EVENTS[i] (the interpreter's KORB_TRE_*)
    @events.each { |e| @mask |= e == :a_call ? 0xa8 : e == :a_return ? 0x150 : 1 << EVENTS.index(e) }
  end

  def __mask; @mask; end
  def __targeted?; !@target_key.nil?; end

  def __deliver(ev, path, lineno, slf, mid, cid, dcls, bind, val, key, params)
    return unless @enabled && @mask[EVENTS.index(ev)] == 1
    return if @thread && !@thread.equal?(Thread.current)
    return if @target_key && !(key && key.include?(@target_key))
    return if @target_line && (ev != :line || lineno != @target_line)
    outer = @frame                                # non-nil only under TracePoint.allow_reentry
    @frame = { event: ev, path: path, lineno: lineno, self: slf, method_id: mid, callee_id: cid,
               defined_class: dcls, binding: bind, value: val, params: params,
               eval_script: (ev == :script_compiled ? val : nil) }
    begin
      @block.call(self)
    ensure
      @frame = outer
    end
  end

  def self.__sym(e)
    raise TypeError, "no implicit conversion of #{e.class} into Symbol" unless e.respond_to?(:to_sym)
    s = e.to_sym
    return s if s.is_a?(Symbol)
    raise TypeError, "can't convert #{e.class} into Symbol (#{e.class}#to_sym gives #{s.class})"
  end

  def self.trace(*events, &block)
    tp = new(*events, &block)
    tp.enable
    tp
  end

  # inside a hook: events fire again for the block
  def self.allow_reentry
    raise RuntimeError, "No need to allow reentrance." unless __trace_reentry(false)
    begin
      yield
    ensure
      __trace_reentry(true)
    end
  end

  def enabled?; @enabled; end

  # 検証順は CRuby 実測に合わせる: only target_line → target 種別 →
  # line event 未登録 → Integer 変換 → target_thread → nest → hook 不成立。
  def enable(target: nil, target_line: nil, target_thread: :default, &block)
    raise ArgumentError, "only target_line is specified" if target_line && !target
    if target && !(target.is_a?(Proc) || target.is_a?(Method) || target.is_a?(UnboundMethod))
      raise ArgumentError, "specified target is not supported"
    end
    if target_line
      unless @events.include?(:line)
        raise ArgumentError, "target_line is specified, but line event is not specified"
      end
      unless target_line.is_a?(Integer)
        target_line = target_line.to_int if target_line.respond_to?(:to_int)
        unless target_line.is_a?(Integer)
          raise TypeError, "no implicit conversion of #{target_line.class} into Integer"
        end
      end
    end
    unless target_thread.equal?(:default) || target_thread.nil? || target_thread.is_a?(Thread)
      raise TypeError, "wrong argument type #{target_thread.class} (expected VM/thread)"
    end
    if @enabled && (target || @target)
      raise ArgumentError, "can't nest-enable a targeting TracePoint"
    end
    raise ArgumentError, "can not enable any hooks" if target && !__hookable?(target, target_line)
    th = target_thread.equal?(:default) ? (block ? Thread.current : nil) : target_thread
    __swap(true, target, target_line, block, th)
  end

  def disable(&block)
    if block && @enabled && @target
      raise ArgumentError, "can't disable a targeting TracePoint in a block"
    end
    __swap(false, nil, nil, block)
  end

  def inspect
    f = @frame
    return "#<TracePoint:#{@enabled ? 'enabled' : 'disabled'}>" unless f
    ev = f[:event]
    if ev == :thread_begin || ev == :thread_end || ev == :fiber_switch
      "#<TracePoint:#{ev} #{f[:self].inspect}>"
    elsif f[:method_id]
      "#<TracePoint:#{ev} '#{f[:method_id]}' #{f[:path]}:#{f[:lineno]}>"
    else
      "#<TracePoint:#{ev} #{f[:path]}:#{f[:lineno]}>"
    end
  end

  [:event, :lineno, :path, :method_id, :callee_id, :defined_class, :self,
   :binding, :eval_script, :instruction_sequence].each do |m|
    define_method(m) do
      f = @frame
      raise RuntimeError, "access from outside" unless f
      f[m]
    end
  end

  { return_value: %i[return b_return c_return], raised_exception: %i[raise rescue] }.each do |m, evs|
    define_method(m) do
      f = @frame or raise RuntimeError, "access from outside"
      raise RuntimeError, "not supported by this event" unless evs.include?(f[:event])
      f[:value]
    end
  end

  def parameters
    f = @frame
    raise RuntimeError, "access from outside" unless f
    case f[:event]
    when :call, :return, :c_call, :c_return
      k = f[:defined_class]
      k && f[:method_id] ? (k.instance_method(f[:callee_id]).parameters rescue []) : []
    when :b_call, :b_return
      f[:params] || []
    else
      raise RuntimeError, "not supported by this event"
    end
  end

  private

  # enable/disable 共通: 状態を差し替え、block 付きならその区間だけ有効にして
  # ensure で元に戻す。block 無しなら直前の enabled? を返す (CRuby)。
  def __swap(on, target, target_line, block, thread = nil)
    prev, prev_t, prev_l, prev_th, prev_k = @enabled, @target, @target_line, @thread, @target_key
    @enabled, @target, @target_line = on, target, target_line
    @target_key = target ? target.__send__(:__trace_key) : nil
    @thread = thread
    on ? TracePoint.__add(self) : TracePoint.__remove(self)
    return prev unless block
    begin
      block.call
    ensure
      @enabled, @target, @target_line, @thread, @target_key = prev, prev_t, prev_l, prev_th, prev_k
      @enabled ? TracePoint.__add(self) : TracePoint.__remove(self)
    end
  end

  # target_line は「target の開始行より前は絶対に無い」下限だけ見る
  # (上限は iseq を持たないと分からない)。
  def __hookable?(target, tl)
    allowed = target.is_a?(Proc) ? PROC_EVENTS : EVENTS
    return false if (@events & allowed).empty?
    return true unless tl
    loc = target.source_location
    !loc.nil? && tl >= loc[1]
  end
end
