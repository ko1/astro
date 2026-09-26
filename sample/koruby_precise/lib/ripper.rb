# Ripper — CRuby では ripper.so (parse.y から生成された C 拡張) だが、koruby は
# prism 自身が持つ翻訳器 Prism::Translation::Ripper を実装に使う。prism の木を
# 歩いて Ripper のイベントを発火する Ruby 実装で、CRuby も同じものを
# prism の default gem として配っている。
#
# 先に EXPR_* と Lexer::State を置く: prism/lex_compat.rb がこれらを参照する。
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
    # Ripper.lex の 4 番目の要素。整数 1 個のラッパで、ビット演算と比較を持つ。
    class State
      attr_reader :to_int
      alias to_i to_int

      def initialize(i)
        @to_int = i
        freeze
      end

      def [](index) = @to_int[index]
      def allbits?(mask) = @to_int.allbits?(mask)
      def anybits?(mask) = @to_int.anybits?(mask)
      def nobits?(mask) = @to_int.nobits?(mask)
      def &(i) = self.class.new(@to_int & i.to_int)
      def |(i) = self.class.new(@to_int | i.to_int)
      def ==(other) = @to_int == other.to_int
      def to_s = Ripper.__state_name(@to_int)
      def inspect = to_s
      def pretty_print(q) = q.text(inspect)
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

  def self.__state_name(bits)
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
      Prism::Translation::Ripper
    end
  end

  def self.const_missing(name)
    return const_set(name, __impl.const_get(name)) if name == :SexpBuilder || name == :SexpBuilderPP
    super
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
    # prism/lex_compat.rb は「本物の Ripper が BOM を先に流すか」を Ripper.lex に
    # 訊きに来る。ここでは Ripper の実体がその lex_compat なので、その 1 回だけは
    # 再入せず CRuby 3.x の答え (流す = 先頭 token は桁 0) を返す。
    BOM_PROBE = "\xEF\xBB\xBF# test"
    BOM_PROBE_ANSWER = [[[1, 0], :on_comment, "# test", Lexer::State.new(EXPR_BEG)]].freeze

    def lex(src, filename = "-", lineno = 1, raise_errors: false)
      return BOM_PROBE_ANSWER if src.is_a?(String) && src.b == BOM_PROBE.b
      s = __source(src)
      __fill_sp(s, __impl.lex(s, filename, lineno, raise_errors: raise_errors))
    end

    # 同梱の prism (lex_compat.rb) は :on_sp を作らない。CRuby の prism 1.9 に
    # 入った post_process_tokens と同じく、トークン間の隙間を空白トークンで
    # 埋める。行継続 (`\\` + 改行) だけは Ripper が 3 つに割るので合わせる。
    def __fill_sp(src, tokens)
      return tokens unless tokens.is_a?(Array)
      # prism の LexCompat::Token は Array の SimpleDelegator。CRuby の Ripper.lex は
      # 素の Array を返す (委譲越しだと token[1] が毎回 method_missing を通る)。
      tokens = tokens.map { |t| t.respond_to?(:__getobj__) ? t.__getobj__ : t }
      return tokens unless src.is_a?(String)
      bom = src.byteslice(0, 3) == "\xEF\xBB\xBF".b
      starts = [0]                                  # 各行の先頭バイトオフセット
      src.each_line { |l| starts << starts.last + l.bytesize }
      state = Lexer::State.new(EXPR_BEG)
      prev_end = bom ? 3 : 0
      out = []
      loc = lambda do |off|                         # バイトオフセット → [行, 桁]
        i = (starts.bsearch_index { |st| st > off } || starts.size) - 1
        i = 0 if i < 0
        col = off - starts[i]
        [i + 1, (i.zero? && bom) ? col - 3 : col]
      end
      tokens.each do |tok|
        line, column = tok[0]
        beg = (starts[line - 1] || 0) + column
        beg += 3 if line == 1 && bom               # 桁は BOM を数えない
        if beg > prev_end
          gap = src.byteslice(prev_end, beg - prev_end)
          gline, gcol = loc.call(prev_end)
          cont = gap.byteindex("\\")
          if cont
            nxt = cont + 1
            nxt += 1 if gap.byteslice(nxt, 1) == "\r"
            nxt += 1
            head = gap.byteslice(0, cont)
            tail = gap.byteslice(nxt, gap.bytesize - nxt) || ""
            out << [[gline, gcol], :on_sp, head, state] unless head.empty?
            out << [[gline, gcol + cont], :on_sp, gap.byteslice(cont, nxt - cont), state]
            out << [[gline + 1, 0], :on_sp, tail, state] unless tail.empty?
          else
            out << [[gline, gcol], :on_sp, gap, state]
          end
        end
        out << tok
        state = tok[3]
        prev_end = beg + tok[2].to_s.bytesize
      end
      # 末尾の空白 (`__END__` の後ろの本文は空白ではないので除かれる)
      if prev_end < src.bytesize
        rest = src.byteslice(prev_end, src.bytesize - prev_end)
        if !rest.empty? && rest.match?(/\A[^\S\n]+\z/)
          rline, rcol = loc.call(prev_end)
          out << [[rline, rcol], :on_sp, rest, state]
        end
      end
      out
    end
    private :__fill_sp
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
    @impl = Ripper.__impl.new(Ripper.__source(src), filename, lineno)
  end

  def parse = @impl.parse
  def error? = @impl.error?
  def filename = @impl.filename
  def lineno = @impl.lineno
  def column = @impl.column
  def state = @impl.state
end
