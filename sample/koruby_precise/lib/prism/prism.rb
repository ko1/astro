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
  KIND_ERRORS_FORMAT = 6
  private_constant :KIND_PARSE, :KIND_LEX, :KIND_PARSE_LEX, :KIND_PARSE_COMMENTS, :KIND_SUCCESS, :KIND_ERRORS_FORMAT

  class << self
    def dump(source, **options)
      code = source_bytes(source)
      format_type = raise_error_format_type(options)
      raise_error(code, options, format_type) if format_type
      dump_common(code, options)
    end

    def dump_file(filepath, **options)
      options[:filepath] = filepath
      dump(File.binread(filepath), **options)
    end

    def lex(code, **options)
      code = source_bytes(code)
      format_type = raise_error_format_type(options)
      result = Serialize.load_lex(code, __prism_dump(KIND_LEX, code, dump_options(options)), options.fetch(:freeze, false))
      raise_error(code, options, format_type) if format_type && result.failure?
      result
    end

    def lex_file(filepath, **options)
      options[:filepath] = filepath
      lex(File.binread(filepath), **options)
    end

    def parse(code, **options)
      code = source_bytes(code)
      format_type = raise_error_format_type(options)
      result = Serialize.load_parse(code, dump_common(code, options), options.fetch(:freeze, false))
      raise_error(code, options, format_type) if format_type && result.failure?
      result
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
      format_type = raise_error_format_type(options)
      raise_error(code, options, format_type) if format_type
      Serialize.load_parse_comments(code, __prism_dump(KIND_PARSE_COMMENTS, code, dump_options(options)),
                                    options.fetch(:freeze, false))
    end

    def parse_file_comments(filepath, **options)
      options[:filepath] = filepath
      parse_comments(File.binread(filepath), **options)
    end

    def parse_lex(code, **options)
      code = source_bytes(code)
      format_type = raise_error_format_type(options)
      result = Serialize.load_parse_lex(code, __prism_dump(KIND_PARSE_LEX, code, dump_options(options)),
                                        options.fetch(:freeze, false))
      raise_error(code, options, format_type) if format_type && result.failure?
      result
    end

    def parse_lex_file(filepath, **options)
      options[:filepath] = filepath
      parse_lex(File.binread(filepath), **options)
    end

    def parse_success?(code, **options)
      code = source_bytes(code)
      format_type = raise_error_format_type(options)
      success = __prism_dump(KIND_SUCCESS, code, dump_options(options))
      raise_error(code, options, format_type) if format_type && !success
      success
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
      dump(source, **options)
      nil
    end

    def profile_file(filepath, **options)
      options[:filepath] = filepath
      profile(File.binread(filepath), **options)
    end

    private

    # C 拡張は StringValue() でソースを取る: String そのものでなくてもよい
    # (rspec は EncodedString というラッパを渡してくる)。
    # Serialize の Source は元の encoding のまま渡す (prism/ffi.rb と同じ)。
    def source_bytes(source)
      String.try_convert(source) || source.to_s
    end

    def dump_common(code, options)
      dumped = __prism_dump(KIND_PARSE, code, dump_options(options))
      dumped.freeze if options.fetch(:freeze, false)
      dumped
    end

    # raise_error: の値を prism の pm_errors_format_type_t に (prism/ffi.rb と同じ判定)
    def raise_error_format_type(options)
      case (value = options.delete(:raise_error))
      when nil, false
        nil
      when true
        if $stderr.respond_to?(:tty?) && $stderr.tty?
          no_color = ENV["NO_COLOR"]
          (no_color.nil? || no_color.empty?) ? 3 : 2
        else
          1
        end
      when :plain then 1
      when :style then 2
      when :color then 3
      when Symbol
        raise ArgumentError, "invalid raise_error value: #{value}"
      else
        raise TypeError, "wrong argument type #{value.class} (expected Symbol)"
      end
    end

    # エラーがあれば prism の整形した文言で例外にする。無ければ nil
    def raise_error(code, options, format_type)
      out = __prism_dump(KIND_ERRORS_FORMAT, code, dump_options(options), format_type)
      return if out.nil?

      level = out.getbyte(0)
      encoding_name, _, message = out.byteslice(1, out.bytesize - 1).partition("\0")
      case level
      when 0 # syntax
        error = SyntaxError.new(message.force_encoding(encoding_name))
        error.instance_variable_set(:@path, options.fetch(:filepath, ""))
        raise error
      when 1 # argument
        raise ArgumentError, message.force_encoding(encoding_name)
      when 2 # load
        error = LoadError.new(message.force_encoding(Encoding.find("locale")))
        error.instance_variable_set(:@path, nil)
        raise error
      else
        raise "Unknown error level: #{level}"
      end
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

    # "current" は走っている ruby の文法、"nearest" は知らなければ近い端 (prism/ffi.rb と同じ)
    def dump_options_version(version)
      case version
      when "current"
        version_string_to_number(RUBY_VERSION) || raise(CurrentVersionError, RUBY_VERSION)
      when "latest", nil
        0
      when "nearest"
        dump = version_string_to_number(RUBY_VERSION)
        return dump if dump
        RUBY_VERSION < "3.3" ? version_string_to_number("3.3") : 0
      else
        version_string_to_number(version) || raise(ArgumentError, "invalid version: #{version}")
      end
    end

    def version_string_to_number(version)
      case version
      when /\A3\.3(\.\d+)?\z/ then 1
      when /\A3\.4(\.\d+)?\z/ then 2
      when /\A3\.5(\.\d+)?\z/, /\A4\.0(\.\d+)?\z/ then 3
      when /\A4\.1(\.\d+)?\z/ then 4
      end
    end

    DUMP_OPTIONS_KEYS = [:command_line, :encoding, :filepath, :freeze, :frozen_string_literal, :line, :main_script, :partial_script, :scopes, :version].freeze
    private_constant :DUMP_OPTIONS_KEYS

    # options を prism の受け取るバイト列に詰める (prism/ffi.rb と同じ順序)。
    def dump_options(options)
      unknown_keys = options.keys - DUMP_OPTIONS_KEYS
      raise ArgumentError, "unknown keyword: #{unknown_keys.first}" unless unknown_keys.empty?

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
