# psych.so (libyaml の C 拡張) の代わり。
#
# CRuby の psych は C 側が Psych::Parser#_native_parse と Psych::Emitter の
# 2 つだけを出していて、残り (nodes / visitors / tree_builder …) は全部 Ruby。
# koruby は libyaml のイベント列を builtin (__yaml_parse / __yaml_emit) で
# やり取りし、そこだけ Ruby で組む。lib/psych/ の残りは CRuby のものそのまま。
#
# イベントのタプルは builtins/yaml_bridge.c の並び。末尾 4 要素は
# start_line, start_column, end_line, end_column。

require_relative 'handler'
require_relative 'syntax_error'
require_relative 'class_loader'
require_relative 'visitors'

module Psych
  # libyaml のバージョン。値そのものを使う利用者はほぼ居ないが、
  # Psych::LIBYAML_VERSION が join するので 3 要素の配列で返す。
  def self.libyaml_version
    [0, 2, 5]
  end

  class ClassLoader
    # rb_path_to_class: "A::B" を定数のネストとしてだけ辿る (継承は見ない)。
    private def path2class(path)
      name = path.to_s
      raise ArgumentError, "empty class path" if name.empty?
      raise ArgumentError, "can't retrieve anonymous class #{name}" if name.include?("#<")
      name.split("::", -1).inject(::Object) do |mod, part|
        raise ArgumentError, "empty class path" if part.empty?
        raise NameError.new("undefined class/module #{name}", part.to_sym) unless
          mod.is_a?(::Module) && mod.const_defined?(part, false)
        mod.const_get(part, false)
      end.tap do |klass|
        raise TypeError, "#{name} does not refer to class/module" unless klass.is_a?(::Module)
      end
    end
  end

  module Visitors
    class ToRuby
      # 例外は allocate してから message だけ入れる (#initialize は通さない:
      # 独自の initialize が引数を要求することがある)。CRuby は ivar "mesg" を
      # 直接書くが、koruby の例外はメッセージを専用フィールドに持つ。
      private def build_exception(klass, message)
        e = klass.allocate
        e.__send__(:__set_message, message) if e.respond_to?(:__set_message, true)
        e
      end
    end
  end

  class Parser
    ANY      = 0
    UTF8     = 1
    UTF16LE  = 2
    UTF16BE  = 3

    def _native_parse handler, yaml, path
      src = __yaml_source(yaml)
      events = __yaml_parse(src)
      if events[0] && events[0][0] == :error
        _, problem, line, column = events[0]
        @mark = Mark.new(0, line - 1, column - 1)
        raise Psych::SyntaxError.new(path, line, column, 0, problem, nil)
      end
      located = handler.respond_to?(:event_location)
      events.each do |ev|
        # 末尾 6 要素は start(index, line, column) / end(index, line, column)
        handler.event_location(ev[-5], ev[-4], ev[-2], ev[-1]) if located
        @mark = Mark.new(ev[-3], ev[-2], ev[-1])
        case ev[0]
        when 0 then handler.start_stream(UTF8)
        when 1 then handler.start_document(ev[1] || [], ev[2], ev[3])
        when 2 then handler.end_document(ev[1])
        when 3 then handler.alias(ev[1])
        when 4 then handler.scalar(ev[1], ev[2], ev[3], ev[4], ev[5], ev[6])
        when 5 then handler.start_sequence(ev[1], ev[2], ev[3], ev[4])
        when 6 then handler.end_sequence
        when 7 then handler.start_mapping(ev[1], ev[2], ev[3], ev[4])
        when 8 then handler.end_mapping
        when 9 then handler.end_stream
        end
      end
      handler
    end

    def mark
      @mark || Mark.new(0, 0, 0)
    end

    # CRuby の C 拡張と同じ入力規約: String そのもの、#read を持つもの
    # (1 引数で呼び、String 以外が返ったら TypeError)、それ以外は TypeError。
    private def __yaml_source yaml
      if ::String === yaml
        src = yaml
      elsif yaml.respond_to?(:read)
        src = +""
        loop do
          chunk = yaml.read(65536)
          break if chunk.nil?
          raise TypeError, "instance of #{chunk.class} is not a string" unless ::String === chunk
          break if chunk.empty?
          src << chunk
        end
      else
        raise TypeError, "no implicit conversion of #{yaml.class} into String"
      end
      enc = src.encoding
      return src if enc == Encoding::UTF_8 || enc == Encoding::BINARY || enc == Encoding::US_ASCII
      src.encode(Encoding::UTF_8)
    end
  end

  # イベントを溜めて end_stream でまとめて emit する。libyaml の emitter は
  # 同じ列から同じ出力を作るので、逐次書きとの違いは書き込みのタイミングだけ。
  class Emitter < Psych::Handler
    attr_accessor :line_width, :indentation, :canonical

    def initialize io, options = nil
      @io          = io
      @events      = []
      @indentation = options ? options.indentation : 2
      @line_width  = options ? options.line_width  : 0
      @canonical   = options ? options.canonical   : false
    end

    def start_stream encoding
      raise TypeError, "no implicit conversion into Integer" unless ::Integer === encoding
      @events << [0, encoding]
    end

    private def __yaml_opt_str v
      return nil if v.nil?
      raise TypeError, "no implicit conversion into String" unless ::String === v
      v
    end
    public

    def start_document version, tag_directives, implicit
      raise TypeError, "expected Array" unless ::Array === version && ::Array === tag_directives
      tag_directives.each do |t|
        raise TypeError, "expected Array of two String" unless ::Array === t && t.length == 2
      end
      @events << [1, (version.empty? ? nil : version), tag_directives, implicit]
    end

    def end_document implicit_end = true
      @events << [2, implicit_end]
    end

    def alias anchor
      @events << [3, anchor]
    end

    def scalar value, anchor, tag, plain, quoted, style
      raise TypeError, "no implicit conversion into String" unless ::String === value
      @events << [4, value, __yaml_opt_str(anchor), __yaml_opt_str(tag), plain, quoted, style]
    end

    def start_sequence anchor, tag, implicit, style
      @events << [5, __yaml_opt_str(anchor), __yaml_opt_str(tag), implicit, style]
    end

    def end_sequence
      @events << [6]
    end

    def start_mapping anchor, tag, implicit, style
      @events << [7, __yaml_opt_str(anchor), __yaml_opt_str(tag), implicit, style]
    end

    def end_mapping
      @events << [8]
    end

    def end_stream
      @events << [9]
      out = __yaml_emit(@events, [@indentation, @line_width, @canonical])
      @events = []
      @io.write(out)
      @io
    end
  end
end
