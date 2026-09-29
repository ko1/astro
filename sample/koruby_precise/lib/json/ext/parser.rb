# frozen_string_literal: true

# Pure-Ruby port of json 3.0.2's ext/json/ext/parser/parser.c for koruby
# (which has no C extensions).  It aims to be observably identical to the C
# parser: accepted grammar, options, error classes, messages, #line, #column
# and #json_path.  Differences from the C code are structural only:
#
# * Like the C parser this is an explicit loop over a container stack (not
#   recursion), so max_nesting: false copes with very deep input.  The
#   json_path is rebuilt from that stack when a ParserError is raised.
# * Scanning works on a binary copy of the source with byte offsets, so
#   positions, error fragments and line/column are byte-exact like C.
# * JSON::ResumableParser is not ported.
module JSON
  module Ext
    class ParserConfig
      def initialize(opts)
        @max_nesting = 100
        @allow_nan = false
        @allow_trailing_comma = false
        @allow_comments = false
        @allow_control_characters = false
        @allow_invalid_escape = false
        @symbolize_names = false
        @freeze = false
        @on_load = nil
        @allow_duplicate_key = false
        @decimal_class = nil
        @decimal_method = nil
        @native_flags = 0
        return if opts.nil?
        unless Hash === opts
          raise TypeError, "wrong argument type #{JSON::Ext::ParserConfig.__type_name(opts)} (expected Hash)"
        end
        return if opts.empty?

        unknown = nil
        opts.each do |key, val|
          case key
          when :max_nesting then @max_nesting = val ? (Integer === val ? val : val.to_int) : 0
          when :allow_nan then @allow_nan = val ? true : false
          when :allow_trailing_comma then @allow_trailing_comma = val ? true : false
          when :allow_comments then @allow_comments = val ? true : false
          when :allow_control_characters then @allow_control_characters = val ? true : false
          when :allow_invalid_escape then @allow_invalid_escape = val ? true : false
          when :symbolize_names then @symbolize_names = val ? true : false
          when :freeze then @freeze = val ? true : false
          when :on_load then @on_load = val ? val : nil
          when :allow_duplicate_key then @allow_duplicate_key = val ? true : false
          when :decimal_class
            if val
              if val.respond_to?(:try_convert)
                @decimal_class = val
                @decimal_method = :try_convert
              elsif val.respond_to?(:new)
                @decimal_class = val
                @decimal_method = :new
              elsif Class === val
                name = val.name
                if (colon = name.rindex(':'))
                  @decimal_class = Object.const_get(name[0, colon - 1])
                  @decimal_method = name[(colon + 1)..].to_sym
                else
                  @decimal_class = Kernel
                  @decimal_method = name.to_sym
                end
              end
            end
          else
            (unknown ||= []) << key
          end
        end

        __native_setup
        if unknown
          if unknown.size == 1
            raise ArgumentError, "unknown keyword: #{unknown[0]}"
          else
            raise ArgumentError, "unknown keywords: #{unknown.join(', ')}"
          end
        end
      end

      # Flags for __json_parse (bits as in builtins/json_native.c); nil = on_load /
      # decimal_class, which only the Ruby parser implements.
      private def __native_setup # :nodoc:
        @native_flags =
          if @on_load.nil? && @decimal_class.nil?
            (@symbolize_names ? 1 : 0) | (@freeze ? 2 : 0) | (@allow_nan ? 4 : 0) |
              (@allow_trailing_comma ? 8 : 0) | (@allow_comments ? 16 : 0) |
              (@allow_control_characters ? 32 : 0) | (@allow_invalid_escape ? 64 : 0) |
              (@allow_duplicate_key ? 128 : 0)
          end
      end

      def self.__type_name(obj) # :nodoc:
        case obj
        when nil then 'nil'
        when true then 'true'
        when false then 'false'
        else obj.class.to_s
        end
      end

      # Parses +source+ and returns the resulting Ruby object; raises
      # JSON::ParserError on invalid input.  The native parser (builtins/json_native.c)
      # takes the common case; it answers the source itself for anything it leaves to
      # Parse below (errors included, so messages and positions come from here).
      def parse(source)
        if @native_flags
          value = __json_parse(source, @native_flags, @max_nesting)
          return value unless value.equal?(source)
        end
        Parse.new(self, source).run
      end

      attr_reader :max_nesting, :allow_nan, :allow_trailing_comma, :allow_comments, # :nodoc:
                  :allow_control_characters, :allow_invalid_escape, :symbolize_names,
                  :on_load, :allow_duplicate_key, :decimal_class, :decimal_method

      def freeze_values = @freeze # :nodoc:

      # One parse: the source buffer, cursor and nesting depth.
      class Parse # :nodoc:
        NO_DUP = Object.new.freeze   # "no duplicate yet": on_load may turn a key into nil / false (CRuby: Qundef)
        NO_KEY = Object.new.freeze
        STRING_STOP = /["\\\x00-\x1f]/n
        STRING_STOP_NOCTRL = /["\\]/n
        UTF_8 = ::Encoding::UTF_8
        BINARY = ::Encoding::BINARY
        HEX = [].tap do |t|
          256.times { |i| t << -1 }
          10.times { |i| t[48 + i] = i }
          6.times { |i| t[65 + i] = 10 + i; t[97 + i] = 10 + i }
        end.freeze

        def initialize(config, source)
          @max_nesting = config.max_nesting
          @allow_nan = config.allow_nan
          @allow_trailing_comma = config.allow_trailing_comma
          @allow_comments = config.allow_comments
          @allow_control_characters = config.allow_control_characters
          @allow_invalid_escape = config.allow_invalid_escape
          @symbolize_names = config.symbolize_names
          @freeze = config.freeze_values
          @on_load = config.on_load
          @allow_duplicate_key = config.allow_duplicate_key
          @decimal_class = config.decimal_class
          @decimal_method = config.decimal_method
          @stop = @allow_control_characters ? STRING_STOP_NOCTRL : STRING_STOP
          @b = convert_encoding(source)
          @len = @b.bytesize
          @pos = 0
          @nesting = 0
        end

        # Phase of the innermost open container, for json_path (parser.c's
        # frame->phase): expecting a value, a key, or ',' / closing bracket.
        PHASE_VALUE = 0
        PHASE_KEY = 1
        PHASE_COMMA = 2

        # Iterative like parser.c's json_parse_any, so nesting depth is not
        # bounded by the Ruby stack (max_nesting: false).  @stack holds the
        # open Arrays/Hashes; @ostack holds [key, count, dup_key, start] per
        # open Hash.
        def run
          stack = @stack = []
          ostack = @ostack = []
          on_load = @on_load
          b = @b
          c = skip_ws
          while true
            # expecting a value; c is the byte at @pos
            @phase = PHASE_VALUE
            case c
            when 34
              value = parse_string(false)
            when 123
              start = @pos
              @pos = start + 1
              c = skip_ws
              if c == 125
                @pos += 1
                value = {}
                value.freeze if @freeze
              else
                nest_in
                stack << {}
                ostack << (st = [nil, 0, NO_DUP, start])
                c = read_key(c, st)
                next
              end
            when 91
              @pos += 1
              c = skip_ws
              if c == 93
                @pos += 1
                value = []
                value.freeze if @freeze
              else
                nest_in
                stack << []
                next
              end
            when 45, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57
              value = parse_number(c)
            when 110
              match_keyword('null')
              value = nil
            when 116
              match_keyword('true')
              value = true
            when 102
              match_keyword('false')
              value = false
            when 78
              raise_syntax('unexpected token %s') unless @allow_nan
              match_keyword('NaN')
              value = Float::NAN
            when 73
              raise_syntax('unexpected token %s') unless @allow_nan
              match_keyword('Infinity')
              value = Float::INFINITY
            when nil
              raise_syntax('unexpected end of input')
            when 0
              raise_syntax('unexpected NULL byte: %s')
            else
              raise_syntax('unexpected character: %s')
            end
            value = on_load.call(value) if on_load

            # A value is complete: hand it to the enclosing container, closing
            # containers as long as their closing bracket follows.
            while true
              top = stack.last
              if top.nil?
                skip_ws
                raise_syntax('unexpected token at end of stream %s') if @pos < @len
                return value
              end

              if Array === top
                top << value
                @phase = PHASE_COMMA
                c = skip_ws
                if c == 44
                  @pos += 1
                  @phase = PHASE_VALUE
                  c = skip_ws
                  break unless c == 93 && @allow_trailing_comma
                elsif c != 93
                  raise_syntax("expected ',' or ']' after array value")
                end
                @pos += 1
                stack.pop
                @nesting -= 1
                top.freeze if @freeze
                value = on_load ? on_load.call(top) : top
              else
                st = ostack.last
                top[st[0]] = value
                count = st[1] += 1
                st[2] = st[0] if top.size != count && NO_DUP.equal?(st[2])   # the key on_load gave may be nil / false
                @phase = PHASE_COMMA
                c = skip_ws
                if c == 44
                  @pos += 1
                  @phase = PHASE_KEY
                  c = skip_ws
                  unless c == 125 && @allow_trailing_comma
                    c = read_key(c, st)
                    break
                  end
                elsif c != 125
                  raise_syntax("expected ',' or '}' after object value, got: %s")
                end
                @pos += 1
                if !NO_DUP.equal?(dup = st[2]) && !@allow_duplicate_key
                  dup = dup.name if Symbol === dup
                  @pos = st[3]
                  raise_error("duplicate key #{dup.inspect}", dup)
                end
                stack.pop
                ostack.pop
                @nesting -= 1
                top.freeze if @freeze
                value = on_load ? on_load.call(top) : top
              end
            end
          end
        end

        private

        def nest_in
          @nesting += 1
          if @max_nesting != 0 && @max_nesting < @nesting
            raise NestingError, "nesting of #{@nesting} is too deep"
          end
        end

        # Reads a key and the ':' after it; returns the byte starting the value.
        def read_key(c, st)
          @phase = PHASE_KEY
          if c == 34
            key = parse_string(true)
            key = @on_load.call(key) if @on_load
            st[0] = key
            @phase = PHASE_VALUE
          elsif st[1] == 0
            raise_syntax('expected object key, got %s')
          else
            raise_syntax('expected object key, got: %s')
          end
          c = skip_ws
          if c == 58
            @pos += 1
          elsif st[1] == 0
            raise_syntax("expected ':' after object key")
          else
            raise_syntax("expected ':' after object key, got: %s")
          end
          skip_ws
        end

        def match_keyword(word)
          pos = @pos
          n = word.bytesize
          if @b.byteslice(pos, n) == word
            @pos = pos + n
          else
            raise_syntax('unexpected token %s')
          end
        end
        # Returns a binary copy of the source (UTF-8 bytes).
        def convert_encoding(source)
          unless String === source
            if source.respond_to?(:to_str)
              str = source.to_str
              unless String === str
                raise TypeError, "can't convert #{source.class} to String (#{source.class}#to_str gives #{str.class})"
              end
              source = str
            else
              raise TypeError, "no implicit conversion of #{ParserConfig.__type_name(source)} into String"
            end
          end
          enc = source.encoding
          if enc == UTF_8 || enc == BINARY
            source.b
          else
            source.encode(UTF_8).b
          end
        end

        # Skips whitespace and comments; returns the next byte (nil at EOS).
        def skip_ws
          b = @b
          pos = @pos
          while true
            c = b.getbyte(pos)
            if c == 32 || c == 10 || c == 13 || c == 9
              pos += 1
            elsif c == 47
              @pos = pos
              eat_comment
              pos = @pos
            else
              @pos = pos
              return c
            end
          end
        end

        def eat_comment
          start = @pos
          raise_syntax('unexpected token %s') unless @allow_comments
          pos = start + 1
          c = @b.getbyte(pos)
          if c == 47 # '//'
            nl = @b.index("\n", pos)
            @pos = nl ? nl + 1 : @len
          elsif c == 42 # '/*'
            pos += 1
            while true
              star = @b.index('*', pos)
              raise_syntax("unterminated comment, expected closing '*/'", start) unless star
              pos = star + 1
              if @b.getbyte(pos) == 47
                pos += 1
                break
              end
            end
            @pos = pos
          else
            raise_syntax('unexpected token %s', start)
          end
        end

        # @pos at the opening quote.
        def parse_string(is_name)
          b = @b
          start = @pos + 1
          stop = b.byteindex(@stop, start)
          if stop && b.getbyte(stop) == 34
            @pos = stop + 1
            str = b.byteslice(start, stop - start)
            str.force_encoding(UTF_8)
            if is_name
              return @symbolize_names ? str.to_sym : -str
            end
            return @freeze ? -str : str
          end

          # Escapes (or control characters) inside: find the closing quote
          # the way parser.c does, then unescape.
          pos = start
          while stop
            c = b.getbyte(stop)
            if c == 34
              break
            elsif c == 92
              pos = stop + 2
            else
              raise_syntax('invalid ASCII control character in string: %s', stop) unless @allow_control_characters
              pos = stop + 1
            end
            stop = pos < @len ? b.byteindex(@stop, pos) : nil
          end

          unless stop
            @pos = @len
            raise_syntax("unexpected end of input, expected closing \"")
          end

          str = unescape(start, stop)
          @pos = stop + 1
          raise_syntax("unexpected end of input, expected closing \"") unless str

          str.force_encoding(UTF_8)
          if is_name
            @symbolize_names ? str.to_sym : -str
          else
            @freeze ? -str : str
          end
        end

        # Mirrors json_string_unescape(); +p+ is the start of the pending
        # literal run, used as the error position for surrogate errors.
        def unescape(start, stop)
          b = @b
          buf = String.new(capacity: stop - start, encoding: BINARY)
          p = pe = start
          while pe < stop && (pe = b.byteindex('\\', pe)) && pe < stop
            buf << b.byteslice(p, pe - p) if pe > p
            pe += 1
            case b.getbyte(pe)
            when 34, 47 # '"', '/'
              p = pe
            when 92 then buf << "\\"; p = pe += 1
            when 110 then buf << "\n"; p = pe += 1
            when 114 then buf << "\r"; p = pe += 1
            when 116 then buf << "\t"; p = pe += 1
            when 98 then buf << "\b"; p = pe += 1
            when 102 then buf << "\f"; p = pe += 1
            when 117 # 'u'
              pe += 1
              ch = unescape_unicode(pe, stop)
              pe += 3
              if (ch & 0xFC00) == 0xD800
                pe += 1
                if pe <= stop - 6 && b.getbyte(pe) == 92 && b.getbyte(pe + 1) == 117
                  sur = unescape_unicode(pe + 2, stop)
                  if (sur & 0xFC00) != 0xDC00
                    raise_syntax('invalid surrogate pair at %s', p)
                  end
                  ch = (((ch & 0x3F) << 10) | ((((ch >> 6) & 0xF) + 1) << 16) | (sur & 0x3FF))
                  pe += 5
                else
                  raise_syntax('incomplete surrogate pair at %s', p)
                end
              elsif (ch & 0xFC00) == 0xDC00
                raise_syntax('unpaired trailing surrogate at %s', p)
              end
              append_utf8(buf, ch)
              p = pe += 1
            when 0
              return nil
            else
              c = b.getbyte(pe)
              if c < 0x20 && !@allow_control_characters
                if c == 10
                  raise_syntax('Invalid unescaped newline character (\\n) in string: %s', pe - 1)
                end
                raise_syntax('invalid ASCII control character in string: %s', pe - 1)
              end
              if @allow_invalid_escape
                buf << c
                p = pe += 1
              else
                raise_syntax('invalid escape character in string: %s', pe - 1)
              end
            end
          end
          buf << b.byteslice(p, stop - p) if stop > p
          buf
        end

        def unescape_unicode(sp, spe)
          if sp > spe - 4
            raise_syntax('incomplete unicode character escape sequence at %s', sp - 2)
          end
          b = @b
          b0 = HEX[b.getbyte(sp)]
          b1 = HEX[b.getbyte(sp + 1)]
          b2 = HEX[b.getbyte(sp + 2)]
          b3 = HEX[b.getbyte(sp + 3)]
          if b0 < 0 || b1 < 0 || b2 < 0 || b3 < 0
            raise_syntax('incomplete unicode character escape sequence at %s', sp - 2)
          end
          (b0 << 12) | (b1 << 8) | (b2 << 4) | b3
        end

        def append_utf8(buf, ch)
          if ch <= 0x7F
            buf << ch
          elsif ch <= 0x7FF
            buf << ((ch >> 6) | 0xC0) << ((ch & 0x3F) | 0x80)
          elsif ch <= 0xFFFF
            buf << ((ch >> 12) | 0xE0) << (((ch >> 6) & 0x3F) | 0x80) << ((ch & 0x3F) | 0x80)
          else
            buf << ((ch >> 18) | 0xF0) << (((ch >> 12) & 0x3F) | 0x80) <<
              (((ch >> 6) & 0x3F) | 0x80) << ((ch & 0x3F) | 0x80)
          end
        end

        # +c+ is the first byte ('-' or a digit).
        def parse_number(c)
          b = @b
          start = @pos
          pos = start
          negative = c == 45
          pos += 1 if negative
          first = b.getbyte(pos)
          dstart = pos
          while (d = b.getbyte(pos)) && d >= 48 && d <= 57
            pos += 1
          end
          ndigits = pos - dstart
          integer = true

          invalid = (first == 48 && ndigits > 1) || (negative && ndigits == 0)
          unless invalid
            if d == 46 # '.'
              integer = false
              pos += 1
              fstart = pos
              while (d = b.getbyte(pos)) && d >= 48 && d <= 57
                pos += 1
              end
              invalid = true if pos == fstart
            end
          end
          unless invalid
            if d == 101 || d == 69 # 'e', 'E'
              integer = false
              pos += 1
              d = b.getbyte(pos)
              pos += 1 if d == 45 || d == 43
              estart = pos
              while (d = b.getbyte(pos)) && d >= 48 && d <= 57
                pos += 1
              end
              invalid = true if pos == estart
            end
          end

          if invalid
            if @allow_nan && negative && b.getbyte(pos) == 73 # 'I'
              match_keyword('-Infinity')
              return -Float::INFINITY
            end
            raise_syntax('invalid number: %s', start)
          end

          @pos = pos
          text = b.byteslice(start, pos - start)
          if integer
            text.to_i
          elsif @decimal_class
            @decimal_class.__send__(@decimal_method, text)
          else
            text.to_f
          end
        end

        # ---- errors --------------------------------------------------------

        def raise_syntax(format, at = @pos)
          @pos = at
          prefix, suffix = format.split('%s', 2)
          raise_error(suffix ? "#{prefix}#{fragment}#{suffix}" : prefix, NO_KEY)
        end

        def raise_error(message, dup_key)
          line, column = cursor_position
          message = message.b << " at line #{line} column #{column}"
          message.force_encoding(UTF_8)
          e = ParserError.new(message)
          e.instance_variable_set(:@line, line)
          e.instance_variable_set(:@column, column)
          e.instance_variable_set(:@json_path, json_path(dup_key))
          raise e
        end

        # parser.c's json_path_new(): one segment per open container, outermost
        # first, stopping at an object that is between a ',' and its next key.
        def json_path(dup_key)
          path = []
          stack = @stack
          return path unless stack
          last = stack.size - 1
          oi = 0
          stack.each_with_index do |frame, i|
            phase = i == last ? @phase : PHASE_VALUE
            if Array === frame
              path << (phase == PHASE_COMMA ? frame.size - 1 : frame.size)
            else
              st = @ostack[oi]
              oi += 1
              if i == last && !dup_key.equal?(NO_KEY)
                path << dup_key
              elsif phase == PHASE_KEY
                break
              else
                path << st[0]
              end
            end
          end
          path
        end

        # Up to 32 bytes at the cursor, cut at whitespace/NUL and before a
        # trailing multibyte character, quoted; "EOF" at the end.
        def fragment
          pos = @pos
          return 'EOF' if pos >= @len
          b = @b
          n = 0
          while n < 32
            c = b.getbyte(pos + n)
            break if c.nil? || c == 0 || c == 10 || c == 32 || c == 9 || c == 13
            n += 1
          end
          if n == 0
            nul = b.index("\0", pos) || @len
            return b.byteslice(pos, nul - pos)
          end
          n -= 1 while n > 0 && (c = b.getbyte(pos + n - 1)) >= 0x80 && c < 0xC0
          n -= 1 if n > 0 && b.getbyte(pos + n - 1) >= 0xC0
          "'".b << b.byteslice(pos, n) << "'"
        end

        def cursor_position
          pos = @pos
          b = @b
          newlines = pos > 0 ? b.byteslice(0, pos).count("\n") : 0
          if pos < @len && b.getbyte(pos) == 10
            [newlines + 2, 0]
          else
            last_nl = pos > 0 ? b.rindex("\n", pos - 1) : nil
            [newlines + 1, pos - (last_nl || -1)]
          end
        end
      end
    end

    class Parser
      DEFAULT_CONFIG = ParserConfig.new(nil).freeze # :nodoc:
      private_constant :DEFAULT_CONFIG

      def self.parse(source, opts)
        config = (opts.nil? || (Hash === opts && opts.empty?)) ? DEFAULT_CONFIG : ParserConfig.new(opts)
        config.parse(source)
      end
    end
  end
end
