# TracePoint — オブジェクトモデル / API 層のみ。
#
# イベントの発火はインタプリタ側の hook が要る。koruby は「無効時にゼロコスト」
# を崩さずに :line を出す手立てがまだ無いので、ここでは new/enable/disable/
# enabled?/inspect と引数検証だけを CRuby 互換で実装し、ハンドラは呼ばれない。
# 発火が入るときは __handler_frame に 1 フレーム分の情報 (Hash) を積んで、
# 下の reader 群がそこから読む形にすればよい。
class TracePoint
  EVENTS = [:line, :class, :end, :call, :return, :c_call, :c_return,
            :b_call, :b_return, :a_call, :a_return,
            :thread_begin, :thread_end, :fiber_switch,
            :raise, :rescue, :script_compiled].freeze

  # :a_call / :a_return は複数イベントの別名 (CRuby)。
  ALIASES = { a_call: [:call, :b_call, :c_call],
              a_return: [:return, :b_return, :c_return] }.freeze

  # ある target で仕掛けられるイベント。Proc の iseq は method の :call/:return
  # を持てない — CRuby はこれを iseq 走査で判定するが、ここは種別で近似する。
  PROC_EVENTS   = [:line, :b_call, :b_return, :c_call, :c_return,
                   :raise, :rescue, :class, :end].freeze
  METHOD_EVENTS = [:line, :call, :return, :b_call, :b_return, :c_call, :c_return,
                   :raise, :rescue, :class, :end].freeze

  def self.__event_sym(e)
    return e if e.is_a?(Symbol)
    unless e.respond_to?(:to_sym)
      raise TypeError, "no implicit conversion of #{e.class} into Symbol"
    end
    s = e.to_sym
    unless s.is_a?(Symbol)
      raise TypeError, "can't convert #{e.class} to Symbol (#{e.class}#to_sym gives #{s.class})"
    end
    s
  end
  private_class_method :__event_sym

  def initialize(*events, &block)
    events = [:line] if events.empty?
    syms = events.map { |e| TracePoint.send(:__event_sym, e) }
    syms.each do |s|
      raise ArgumentError, "unknown event: #{s}" unless EVENTS.include?(s)
    end
    raise ArgumentError, "must be called with a block" unless block
    @events = syms
    @expanded = syms.flat_map { |s| ALIASES[s] || [s] }
    @block = block
    @enabled = false
    @target = nil
    @target_line = nil
    @frame = nil
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

  def enabled?
    @enabled
  end

  def enable(target: nil, target_line: nil, target_thread: :default, &block)
    # 検証順は CRuby に合わせる (ruby -e で確認):
    #   only target_line → target 種別 → line event 未登録 → Integer 変換 →
    #   nest → 仕掛けられる hook が無い
    raise ArgumentError, "only target_line is specified" if target_line && !target
    if target && !(target.is_a?(Proc) || target.is_a?(Method) || target.is_a?(UnboundMethod))
      raise ArgumentError, "specified target is not supported"
    end
    tl = nil
    if target_line
      unless @expanded.include?(:line)
        raise ArgumentError, "target_line is specified, but line event is not specified"
      end
      tl = __to_int(target_line)
    end
    unless target_thread.equal?(:default) || target_thread.nil? || target_thread.is_a?(Thread)
      raise TypeError, "wrong argument type #{target_thread.class} (expected VM/thread)"
    end
    if @enabled && (target || @target)
      raise ArgumentError, "can't nest-enable a targeting TracePoint"
    end
    raise ArgumentError, "can not enable any hooks" if target && !__hookable?(target, tl)

    prev = @enabled
    prev_target, prev_line = @target, @target_line
    @enabled = true
    @target = target
    @target_line = tl
    return prev unless block
    begin
      block.call
    ensure
      @enabled = prev
      @target, @target_line = prev_target, prev_line
    end
  end

  def disable(&block)
    if block && @enabled && @target
      raise ArgumentError, "can't disable a targeting TracePoint in a block"
    end
    prev = @enabled
    prev_target, prev_line = @target, @target_line
    @enabled = false
    @target = nil
    @target_line = nil
    return prev unless block
    begin
      block.call
    ensure
      @enabled = prev
      @target, @target_line = prev_target, prev_line
    end
  end

  def inspect
    f = @frame
    return "#<TracePoint:#{@enabled ? 'enabled' : 'disabled'}>" unless f
    ev = f[:event]
    case ev
    when :thread_begin, :thread_end, :fiber_switch
      "#<TracePoint:#{ev} #{f[:self].inspect}>"
    when :call, :return, :c_call, :c_return, :b_call, :b_return
      "#<TracePoint:#{ev} '#{f[:method_id]}' #{f[:path]}:#{f[:lineno]}>"
    else
      "#<TracePoint:#{ev} #{f[:path]}:#{f[:lineno]}>"
    end
  end

  def event;            __frame[:event];            end
  def lineno;           __frame[:lineno];           end
  def path;             __frame[:path];             end
  def method_id;        __frame[:method_id];        end
  def callee_id;        __frame[:callee_id];        end
  def defined_class;    __frame[:defined_class];    end
  def self;             __frame[:self];             end
  def binding;          __frame[:binding];          end
  def return_value;     __frame[:return_value];     end
  def raised_exception; __frame[:raised_exception]; end
  def parameters;       __frame[:parameters];       end
  def eval_script;      __frame[:eval_script];      end
  def instruction_sequence; __frame[:iseq];         end

  private

  def __frame
    f = @frame
    raise RuntimeError, "access from outside" unless f
    f
  end

  def __to_int(v)
    return v if v.is_a?(Integer)
    if v.respond_to?(:to_int)
      i = v.to_int
      return i if i.is_a?(Integer)
    end
    raise TypeError, "no implicit conversion of #{v.class} into Integer"
  end

  # target に対して @events のどれかを仕掛けられるか。target_line は
  # 「target の開始行より前の行は絶対に無い」という下限だけを見る (上限は
  # iseq を持たないと分からない)。
  def __hookable?(target, tl)
    allowed = target.is_a?(Proc) ? PROC_EVENTS : METHOD_EVENTS
    return false if (@expanded & allowed).empty?
    if tl
      loc = target.source_location
      return false if loc.nil? || tl < loc[1]
    end
    true
  end
end
