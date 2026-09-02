# TracePoint — object model / 引数検証だけ。イベント発火はインタプリタ側の
# hook が要り、無効時ゼロコストを保ったまま :line を出す手立てがまだ無い
# (docs/tracepoint.md)。ハンドラは呼ばれないので @frame は常に nil で、
# reader は RuntimeError "access from outside" になる。発火を入れるときは
# 1 イベント分の Hash を @frame に積めば reader/inspect はそのまま動く。
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
    @target = @target_line = @frame = nil
  end

  def self.__sym(e)
    raise TypeError, "no implicit conversion of #{e.class} into Symbol" unless e.respond_to?(:to_sym)
    s = e.to_sym
    return s if s.is_a?(Symbol)
    raise TypeError, "can't convert #{e.class} to Symbol (#{e.class}#to_sym gives #{s.class})"
  end

  def self.trace(*events, &block)
    tp = new(*events, &block)
    tp.enable
    tp
  end

  # ハンドラの中でしか意味を持たない。発火が無い今は常にこの RuntimeError。
  def self.allow_reentry
    raise RuntimeError, "No need to allow reentrance."
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
    __swap(true, target, target_line, block)
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
   :binding, :return_value, :raised_exception, :parameters, :eval_script,
   :instruction_sequence].each do |m|
    define_method(m) do
      f = @frame
      raise RuntimeError, "access from outside" unless f
      f[m]
    end
  end

  private

  # enable/disable 共通: 状態を差し替え、block 付きならその区間だけ有効にして
  # ensure で元に戻す。block 無しなら直前の enabled? を返す (CRuby)。
  def __swap(on, target, target_line, block)
    prev, prev_t, prev_l = @enabled, @target, @target_line
    @enabled, @target, @target_line = on, target, target_line
    return prev unless block
    begin
      block.call
    ensure
      @enabled, @target, @target_line = prev, prev_t, prev_l
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
