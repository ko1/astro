# Ripper — CRuby では ripper.so (parse.y から生成された C 拡張) だが、koruby は
# prism 自身が持つ翻訳器 Prism::Translation::Ripper を実装に使う。prism の木を
# 歩いて Ripper のイベントを発火する Ruby 実装で、CRuby も同じものを
# prism の default gem として配っている。
#
# EXPR_* と Lexer::State は Ripper 側に置く: トークンの状態は CRuby と同じく
# Ripper::Lexer::State であってほしいので、prism の同名クラスをこれに差し替える。
class Ripper
  # lexer state のビット (CRuby の ripper.so と同じ値)
  EXPR_NONE    = 0
  EXPR_BEG     = 1 << 0
  EXPR_END     = 1 << 1
  EXPR_ENDARG  = 1 << 2
  EXPR_ENDFN   = 1 << 3
  EXPR_ARG     = 1 << 4
  EXPR_CMDARG  = 1 << 5
  EXPR_MID     = 1 << 6
  EXPR_FNAME   = 1 << 7
  EXPR_DOT     = 1 << 8
  EXPR_CLASS   = 1 << 9
  EXPR_LABEL   = 1 << 10
  EXPR_LABELED = 1 << 11
  EXPR_FITEM   = 1 << 12
  EXPR_VALUE   = EXPR_BEG
  EXPR_BEG_ANY = EXPR_BEG | EXPR_MID | EXPR_CLASS
  EXPR_ARG_ANY = EXPR_ARG | EXPR_CMDARG
  EXPR_END_ANY = EXPR_END | EXPR_ENDARG | EXPR_ENDFN

  class Lexer
    # Ripper.lex の 4 番目の要素 (CRuby の ripper/lexer.rb と同じ形)。
    # prism/lex_compat.rb は State[i] で引くので、その入口と cache も持つ。
    class State
      attr_reader :to_int, :to_s

      def initialize(i)
        @to_int = i
        @to_s = Ripper.lex_state_name(i)
        freeze
      end

      def [](index)
        case index
        when 0, :to_int then @to_int
        when 1, :to_s then @to_s
        end
      end

      alias to_i to_int
      alias inspect to_s
      def pretty_print(q) = q.text(to_s)
      def ==(i) = super || to_int == i
      def &(i) = self.class.new(to_int & i)
      def |(i) = self.class.new(to_int | i)
      def allbits?(i) = to_int.allbits?(i)
      def anybits?(i) = to_int.anybits?(i)
      def nobits?(i) = to_int.nobits?(i)

      STATES = Hash.new { |hash, key| hash[key] = new(key) }
      private_constant :STATES
      def self.[](i) = STATES[i]
    end
  end

  NAMES = {
    EXPR_BEG => "BEG", EXPR_END => "END", EXPR_ENDARG => "ENDARG",
    EXPR_ENDFN => "ENDFN", EXPR_ARG => "ARG", EXPR_CMDARG => "CMDARG",
    EXPR_MID => "MID", EXPR_FNAME => "FNAME", EXPR_DOT => "DOT",
    EXPR_CLASS => "CLASS", EXPR_LABEL => "LABEL", EXPR_LABELED => "LABELED",
    EXPR_FITEM => "FITEM",
  }.freeze
  private_constant :NAMES

  def self.lex_state_name(bits)
    return "NONE" if bits == 0
    NAMES.filter_map { |bit, name| name if bits.anybits?(bit) }.join("|")
  end
end

class Ripper
  # 実体は prism 側。定数 (EXPR_*) は上で置いたものを使う。prism は最初に使う時に
  # 読む: CRuby の ripper.so は prism に依らないので、require "ripper" だけで
  # Prism が定義されてはいけない (simplecov の unloadable_prism)。
  def self.__impl
    @__impl ||= begin
      require "prism"
      require "prism/translation/ripper"
      k = Prism::Translation::Ripper
      k::Lexer.send(:remove_const, :State)
      k::Lexer.const_set(:State, Lexer::State)
      k
    end
  end

  IMPL_CONSTS__ = %i[SexpBuilder SexpBuilderPP PARSER_EVENT_TABLE SCANNER_EVENT_TABLE PARSER_EVENTS SCANNER_EVENTS EVENTS].freeze
  private_constant :IMPL_CONSTS__
  def self.const_missing(name)
    return const_set(name, __impl.const_get(name)) if IMPL_CONSTS__.include?(name)
    super
  end

  # A subclass's own handlers (on_*, compile_error, warn, warning) run on the
  # Ripper object the user made; the translator instance forwards them there.
  FWD_EXTRA__ = %i[compile_error warn warning].freeze
  private_constant :FWD_EXTRA__
  def self.__impl_for(klass)
    (@__impl_for ||= {})[klass] ||= begin
      names = (klass.instance_methods + klass.private_instance_methods).uniq.select do |n|
        (n.start_with?("on_") || FWD_EXTRA__.include?(n)) &&
          !(Ripper <= klass.instance_method(n).owner)   # the user's, not Ripper's or above
      end
      if names.empty?
        __impl
      else
        Class.new(__impl) do
          names.each { |n| define_method(n) { |*a, &b| @__outer.__send__(n, *a, &b) } }
        end
      end
    end
  end

  class << self
    # CRuby の ripper.so は StringValue() でソースを取るので String そのもので
    # なくてもよい (rspec は EncodedString、IO を渡す使い方もある)。prism の
    # 翻訳器は String のメソッドを直に呼ぶので、ここで実体に均しておく。
    def __source(src)
      return src if src.is_a?(String)
      String.try_convert(src) || (src.respond_to?(:read) ? src.read : src.to_s)
    end

    def parse(src, filename = "(ripper)", lineno = 1) = __impl.parse(__source(src), filename, lineno)
    def lex(src, filename = "-", lineno = 1, raise_errors: false)
      __impl.lex(__source(src), filename, lineno, raise_errors: raise_errors)
    end
    def tokenize(src, filename = "-", lineno = 1, raise_errors: false)
      lex(src, filename, lineno, raise_errors: raise_errors).map { |_, _, token, _| token }
    end
    def sexp(src, filename = "-", lineno = 1, raise_errors: false) =
      __impl.sexp(__source(src), filename, lineno, raise_errors: raise_errors)
    def sexp_raw(src, filename = "-", lineno = 1, raise_errors: false) =
      __impl.sexp_raw(__source(src), filename, lineno, raise_errors: raise_errors)
    # Ripper.slice は TokenPattern (ripper/lexer.rb) が要る。prism の翻訳器には
    # 無いので未実装 — 黙って nil を返すと「一致しなかった」と読めてしまう。
    def slice(src, pattern, n = 0)
      raise NotImplementedError, "Ripper.slice is not implemented on koruby"
    end
  end

  def initialize(src, filename = "(ripper)", lineno = 1)
    @impl = Ripper.__impl_for(self.class).new(Ripper.__source(src), filename, lineno)
    @impl.instance_variable_set(:@__outer, self)
  end

  # the default handlers (CRuby's _dispatch_N): a scanner / parser event gives
  # back its first argument, so a subclass's `super` works
  def method_missing(name, *args, &blk)
    return args[0] if name.start_with?("on_") && Ripper.__impl.private_method_defined?(name)
    return nil if FWD_EXTRA__.include?(name)
    super
  end
  def respond_to_missing?(name, priv = false)   # private in CRuby's Ripper too
    (priv && name.start_with?("on_") && Ripper.__impl.private_method_defined?(name)) || super
  end

  def parse = (@parsed = true; @impl.parse)
  def error? = @impl.error?
  def filename = @impl.filename
  def lineno = @impl.lineno
  def column = @impl.column
  def state = @impl.state
  # what the parser has seen so far (the source's own encoding before #parse)
  def encoding = @parsed ? @impl.encoding : @impl.__send__(:source).encoding
  def end_seen? = @parsed ? @impl.end_seen? : false
  attr_accessor :yydebug, :debug_output                # no parser trace to give
end

# ripper/filter.rb: walk the token stream Ripper.lex gives, calling #on_<event>
# (or #on_default) with each token and the running data
class Ripper
  class Filter
    def initialize(src, filename = "-", lineno = 1)
      @__src = src
      @__filename = filename
      @__lineno = lineno
      @__line = nil
      @__col = nil
      @__state = nil
    end

    def filename = @__filename
    def lineno = @__line
    def column = @__col
    def state = @__state

    def parse(init = nil)
      data = init
      Ripper.lex(@__src, @__filename, @__lineno).each do |pos, event, tok, state|
        @__line, @__col = *pos
        @__state = state
        data = if respond_to?(event, true)
                 __send__(event, tok, data)
               else
                 on_default(event, tok, data)
               end
      end
      data
    end

    private

    def on_default(event, token, data) = data
  end
end
