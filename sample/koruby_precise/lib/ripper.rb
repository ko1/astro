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

require "prism"
require "prism/translation/ripper"

class Ripper
  # 実体は prism 側。定数 (EXPR_*) は上で置いたものを使う。
  class << self
    # CRuby の ripper.so は StringValue() でソースを取るので String そのもので
    # なくてもよい (rspec は EncodedString、IO を渡す使い方もある)。prism の
    # 翻訳器は String のメソッドを直に呼ぶので、ここで実体に均しておく。
    def __source(src)
      return src if src.is_a?(String)
      String.try_convert(src) || (src.respond_to?(:read) ? src.read : src.to_s)
    end

    def parse(src, filename = "(ripper)", lineno = 1) = Prism::Translation::Ripper.parse(__source(src), filename, lineno)
    # prism/lex_compat.rb は「本物の Ripper が BOM を先に流すか」を Ripper.lex に
    # 訊きに来る。ここでは Ripper の実体がその lex_compat なので、その 1 回だけは
    # 再入せず CRuby 3.x の答え (流す = 先頭 token は桁 0) を返す。
    BOM_PROBE = "\xEF\xBB\xBF# test"
    BOM_PROBE_ANSWER = [[[1, 0], :on_comment, "# test", Lexer::State.new(EXPR_BEG)]].freeze

    def lex(src, filename = "-", lineno = 1, raise_errors: false)
      return BOM_PROBE_ANSWER if src.is_a?(String) && src.b == BOM_PROBE.b
      Prism::Translation::Ripper.lex(__source(src), filename, lineno, raise_errors: raise_errors)
    end
    def tokenize(src, filename = "-", lineno = 1, raise_errors: false)
      lex(src, filename, lineno, raise_errors: raise_errors).map { |_, _, token, _| token }
    end
    def sexp(src, filename = "-", lineno = 1, raise_errors: false) =
      Prism::Translation::Ripper.sexp(__source(src), filename, lineno, raise_errors: raise_errors)
    def sexp_raw(src, filename = "-", lineno = 1, raise_errors: false) =
      Prism::Translation::Ripper.sexp_raw(__source(src), filename, lineno, raise_errors: raise_errors)
    # Ripper.slice は TokenPattern (ripper/lexer.rb) が要る。prism の翻訳器には
    # 無いので未実装 — 黙って nil を返すと「一致しなかった」と読めてしまう。
    def slice(src, pattern, n = 0)
      raise NotImplementedError, "Ripper.slice is not implemented on koruby"
    end
  end

  SexpBuilder = Prism::Translation::Ripper::SexpBuilder
  SexpBuilderPP = Prism::Translation::Ripper::SexpBuilderPP

  def initialize(src, filename = "(ripper)", lineno = 1)
    @impl = Prism::Translation::Ripper.new(self.class.__source(src), filename, lineno)
  end

  def parse = @impl.parse
  def error? = @impl.error?
  def filename = @impl.filename
  def lineno = @impl.lineno
  def column = @impl.column
  def state = @impl.state
end
