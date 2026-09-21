# prism の C 拡張 (prism/prism.so) の代わり。
#
# CRuby では prism.rb が `require "prism/prism"` で C 拡張を読み、そこが
# Prism.dump / parse / lex / ... を定義する。koruby は拡張の代わりに builtin
# `__prism_dump` 1 本を持ち、あとは prism 自身の Ruby 実装 (Serialize) で木に
# 戻す。FFI backend (prism/ffi.rb) と同じ組み立てで、Fiddle/FFI を要らなくした形。
#
# これで Prism の Ruby ライブラリ一式が無改造で動く。Ripper (lib/ripper.rb) は
# Prism::Translation::Ripper をそのまま使う。

require "prism/serialize"

module Prism
  VERSION = __prism_dump(5, "", nil).freeze   # pm_version()

  KIND_PARSE = 0
  KIND_LEX = 1
  KIND_PARSE_LEX = 2
  KIND_PARSE_COMMENTS = 3
  KIND_SUCCESS = 4
  private_constant :KIND_PARSE, :KIND_LEX, :KIND_PARSE_LEX, :KIND_PARSE_COMMENTS, :KIND_SUCCESS

  class << self
    def dump(source, **options)
      dumped = __prism_dump(KIND_PARSE, source_bytes(source), dump_options(options))
      dumped.freeze if options.fetch(:freeze, false)
      dumped
    end

    def dump_file(filepath, **options)
      options[:filepath] = filepath
      dump(File.binread(filepath), **options)
    end

    def lex(code, **options)
      code = source_bytes(code)
      Serialize.load_lex(code, __prism_dump(KIND_LEX, code, dump_options(options)), options.fetch(:freeze, false))
    end

    def lex_file(filepath, **options)
      options[:filepath] = filepath
      lex(File.binread(filepath), **options)
    end

    def parse(code, **options)
      code = source_bytes(code)
      Serialize.load_parse(code, dump(code, **options), options.fetch(:freeze, false))
    end

    def parse_file(filepath, **options)
      options[:filepath] = filepath
      parse(File.binread(filepath), **options)
    end

    def parse_stream(stream, **options)
      parse(stream.read, **options)
    end

    def parse_comments(code, **options)
      code = source_bytes(code)
      Serialize.load_parse_comments(code, __prism_dump(KIND_PARSE_COMMENTS, code, dump_options(options)),
                                    options.fetch(:freeze, false))
    end

    def parse_file_comments(filepath, **options)
      options[:filepath] = filepath
      parse_comments(File.binread(filepath), **options)
    end

    def parse_lex(code, **options)
      code = source_bytes(code)
      Serialize.load_parse_lex(code, __prism_dump(KIND_PARSE_LEX, code, dump_options(options)),
                               options.fetch(:freeze, false))
    end

    def parse_lex_file(filepath, **options)
      options[:filepath] = filepath
      parse_lex(File.binread(filepath), **options)
    end

    def parse_success?(code, **options)
      __prism_dump(KIND_SUCCESS, source_bytes(code), dump_options(options))
    end

    def parse_failure?(code, **options)
      !parse_success?(code, **options)
    end

    def parse_file_success?(filepath, **options)
      options[:filepath] = filepath
      parse_success?(File.binread(filepath), **options)
    end

    def parse_file_failure?(filepath, **options)
      !parse_file_success?(filepath, **options)
    end

    def profile(source, **options)
      __prism_dump(KIND_PARSE, source_bytes(source), dump_options(options))
      nil
    end

    def profile_file(filepath, **options)
      options[:filepath] = filepath
      profile(File.binread(filepath), **options)
    end

    private

    # C 拡張は StringValue() でソースを取る: String そのものでなくてもよい
    # (rspec は EncodedString というラッパを渡してくる)。
    def source_bytes(source)
      str = String.try_convert(source) || source.to_s
      str.b
    end

    # -X の 1 文字ずつがビット (prism/ffi.rb と同じ表)
    def dump_options_command_line(options)
      command_line = options.fetch(:command_line, "")
      raise ArgumentError, "command_line must be a string" unless command_line.is_a?(String)

      command_line.each_char.inject(0) do |value, char|
        case char
        when "a" then value | 0b000001
        when "e" then value | 0b000010
        when "l" then value | 0b000100
        when "n" then value | 0b001000
        when "p" then value | 0b010000
        when "x" then value | 0b100000
        else raise ArgumentError, "invalid command_line option: #{char}"
        end
      end
    end

    def dump_options_version(version)
      # "current" は「走っている ruby の文法で」の意 (prism の Ripper 翻訳器が
      # 使う)。koruby が抱える prism は最新版なので 3.5 以降は 0 と同じ。
      version = RUBY_VERSION if version == "current"
      case version
      when nil, "latest" then 0
      when /\A3\.3(\.\d+)?\z/ then 1
      when /\A3\.4(\.\d+)?\z/ then 2
      when /\A3\.5(\.\d+)?\z/, /\A4\.\d+(\.\d+)?\z/ then 0
      else raise ArgumentError, "invalid version: #{version}"
      end
    end

    # options を prism の受け取るバイト列に詰める (prism/ffi.rb と同じ順序)。
    def dump_options(options)
      template = +""
      values = []

      template << "L"
      if (filepath = options[:filepath])
        values.push(filepath.bytesize, filepath.b)
        template << "A*"
      else
        values << 0
      end

      template << "l"
      values << options.fetch(:line, 1)

      template << "L"
      if (encoding = options[:encoding])
        name = encoding.is_a?(Encoding) ? encoding.name : encoding
        values.push(name.bytesize, name.b)
        template << "A*"
      else
        values << 0
      end

      template << "C"
      values << (options.fetch(:frozen_string_literal, false) ? 1 : 0)

      template << "C"
      values << dump_options_command_line(options)

      template << "C"
      values << dump_options_version(options[:version])

      template << "C"
      values << (options[:encoding] == false ? 1 : 0)

      template << "C"
      values << (options.fetch(:main_script, false) ? 1 : 0)

      template << "C"
      values << (options.fetch(:partial_script, false) ? 1 : 0)

      template << "C"
      values << (options.fetch(:freeze, false) ? 1 : 0)

      template << "L"
      if (scopes = options[:scopes])
        values << scopes.length

        scopes.each do |scope|
          locals = nil
          forwarding = 0

          case scope
          when Array
            locals = scope
          when Scope
            locals = scope.locals
            scope.forwarding.each do |forward|
              case forward
              when :*     then forwarding |= 0x1
              when :**    then forwarding |= 0x2
              when :&     then forwarding |= 0x4
              when :"..." then forwarding |= 0x8
              else raise ArgumentError, "invalid forwarding value: #{forward}"
              end
            end
          else
            raise TypeError, "wrong argument type #{scope.class.inspect} (expected Array or Prism::Scope)"
          end

          template << "L"
          values << locals.length

          template << "C"
          values << forwarding

          locals.each do |local|
            name = local.name
            template << "L"
            values << name.bytesize
            template << "A*"
            values << name.b
          end
        end
      else
        values << 0
      end

      values.pack(template)
    end
  end
end
