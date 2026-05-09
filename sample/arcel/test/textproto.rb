# Minimal textproto parser tailored to Google's cel-spec test corpus.
#
# What it handles (everything cel-spec/tests/simple/testdata/*.textproto uses):
#   key: value                  scalar field
#   key: { ... }                submessage (with explicit ':')
#   key { ... }                 submessage (':' omitted)
#   key: [v1, v2, ...]          repeated scalar via list literal
#   key: < ... >                submessage with angle-bracket form
#   "...."  '....'              string literal (C-ish escapes, adjacent literals concat)
#   123  -123  0  0u  0U  -0    integer (uint suffix tolerated)
#   1.5  -1e-3                  float
#   true  false                 bool
#   NULL_VALUE  IDENT_LIKE      bare identifier (enum)
#   # comment                   line comment
#
# Repeated fields (`section`, `test`, `errors`, `bindings`, `entries`,
# `values`, ...) appear multiple times in the textproto.  The parser
# *always* returns each field as an Array, so callers can iterate
# uniformly and just do `[0]` for fields they expect to be singular.
#
# A submessage is represented as a `TextProto::Message`, which is a
# thin wrapper around `Hash{String => Array}` with `[]` and
# `each_pair` for ergonomic access.  Scalars are returned as Ruby
# primitives (Integer / Float / String / true / false / nil for null /
# Symbol for bare identifiers other than true/false).

module TextProto
  class ParseError < StandardError; end

  class Message
    def initialize
      @fields = {}
    end

    def add(key, value)
      (@fields[key] ||= []) << value
    end

    # All values of a field as an array (empty if absent).
    def [](key)
      @fields[key] || []
    end

    # First value of a field, or nil.  Convenient for "I know this is scalar".
    def first(key)
      v = @fields[key]
      v && v.first
    end

    def has?(key)
      @fields.key?(key)
    end

    def keys
      @fields.keys
    end

    def each_pair(&blk) = @fields.each_pair(&blk)

    def to_h = @fields.dup
  end

  # ---- tokenizer -----------------------------------------------------------

  class Lexer
    def initialize(src, path = '<input>')
      # Force binary encoding — textproto strings may contain arbitrary
      # bytes, and we want @pos to be a *byte* offset that matches
      # @src.bytesize.  With UTF-8 encoding, @src[@pos] does character
      # indexing and silently returns nil past the last codepoint even
      # though byte position is still in range.
      @src  = src.b
      @pos  = 0
      @len  = @src.bytesize
      @path = path
      @line = 1
    end

    def loc = "#{@path}:#{@line}"

    def peek
      skip_ws
      @pos < @len ? @src[@pos] : nil
    end

    # Returns [type, value, loc]
    def next_token
      skip_ws
      return [:eof, nil, loc] if @pos >= @len

      ch = @src[@pos]
      case ch
      when '{' then @pos += 1; [:lbrace, '{', loc]
      when '}' then @pos += 1; [:rbrace, '}', loc]
      when '<' then @pos += 1; [:lbrace, '<', loc]
      when '>' then @pos += 1; [:rbrace, '>', loc]
      when '[' then @pos += 1; [:lbracket, '[', loc]
      when ']' then @pos += 1; [:rbracket, ']', loc]
      when ',' then @pos += 1; [:comma, ',', loc]
      when ':' then @pos += 1; [:colon, ':', loc]
      when '"', "'"
        [:string, read_string(ch), loc]
      when '-', '+', '0'..'9'
        # Special floats: -inf, +inf, -nan, +nan (bare 'inf'/'nan' handled
        # in the ident branch below).
        if (ch == '-' || ch == '+') && @src.byteslice(@pos + 1, 3) =~ /\A(?:inf|nan)\b/i
          sign = ch
          @pos += 1
          name = read_ident
          val = name.downcase.start_with?('inf') ? Float::INFINITY : Float::NAN
          val = -val if sign == '-'
          return [:number, val, loc]
        end
        [:number, read_number, loc]
      else
        if ch =~ /[A-Za-z_]/
          tok = read_ident
          # bare inf/nan as float
          if tok == 'inf' || tok == 'Inf' || tok == 'INF' ||
             tok == 'infinity' || tok == 'Infinity' || tok == 'INFINITY'
            return [:number, Float::INFINITY, loc]
          end
          if tok == 'nan' || tok == 'NaN' || tok == 'NAN'
            return [:number, Float::NAN, loc]
          end
          [:ident, tok, loc]
        else
          raise ParseError, "#{loc}: unexpected character #{ch.inspect}"
        end
      end
    end

    # Public — used by parser when entering proto extension/Any field
    # `[some/dotted.path/Name]`.  Returns the bracketed string (with
    # leading '[' and trailing ']' included so it serves as a unique
    # field key).  Caller has just consumed the opening '['.
    def slurp_until_rbracket
      start = @pos
      while @pos < @len && @src[@pos] != ']'
        @line += 1 if @src[@pos] == "\n"
        @pos += 1
      end
      raise ParseError, "#{loc}: unterminated '[...]'" if @pos >= @len
      key = @src[start...@pos]
      @pos += 1   # consume ']'
      "[#{key}]"
    end

    private

    def skip_ws
      while @pos < @len
        ch = @src[@pos]
        if ch == "\n"
          @line += 1
          @pos += 1
        elsif ch =~ /\s/
          @pos += 1
        elsif ch == '#'
          # comment to end of line
          @pos += 1
          @pos += 1 while @pos < @len && @src[@pos] != "\n"
        else
          break
        end
      end
    end

    def read_string(quote)
      buf = +''
      loop do
        @pos += 1                  # skip opening quote (or the next opening one)
        until @pos >= @len || @src[@pos] == quote
          c = @src[@pos]
          if c == '\\'
            @pos += 1
            esc = @src[@pos]
            case esc
            when 'n'  then buf << "\n"
            when 't'  then buf << "\t"
            when 'r'  then buf << "\r"
            when '\\' then buf << '\\'
            when '"'  then buf << '"'
            when "'"  then buf << "'"
            when '0'  then buf << "\0"
            when 'a'  then buf << "\a"
            when 'b'  then buf << "\b"
            when 'f'  then buf << "\f"
            when 'v'  then buf << "\v"
            when 'x'
              h = @src[@pos + 1, 2]
              raise ParseError, "#{loc}: bad \\x escape" unless h =~ /\A[0-9A-Fa-f]{2}\z/
              buf << h.to_i(16).chr
              @pos += 2
            when 'u', 'U'
              # \uXXXX (4 hex) or \UXXXXXXXX (8 hex) → UTF-8.  cel-spec
              # source uses these for non-ASCII codepoints; without
              # converting here the literal escape leaks through to the
              # harness as `\U0001F431` text.
              width = (esc == 'u') ? 4 : 8
              hex = @src[@pos + 1, width]
              raise ParseError, "#{loc}: bad \\#{esc} escape" unless hex =~ /\A[0-9A-Fa-f]{#{width}}\z/
              cp = hex.to_i(16)
              # encode as UTF-8 bytes (binary-safe append)
              if    cp < 0x80
                buf << cp.chr
              elsif cp < 0x800
                buf << (0xC0 | (cp >> 6)).chr << (0x80 | (cp & 0x3F)).chr
              elsif cp < 0x10000
                buf << (0xE0 | (cp >> 12)).chr << (0x80 | ((cp >> 6) & 0x3F)).chr << (0x80 | (cp & 0x3F)).chr
              else
                buf << (0xF0 | (cp >> 18)).chr << (0x80 | ((cp >> 12) & 0x3F)).chr << (0x80 | ((cp >> 6) & 0x3F)).chr << (0x80 | (cp & 0x3F)).chr
              end
              @pos += width
            when '0'..'7'
              # 1- to 3-digit octal
              j = @pos
              while @pos < j + 3 && @src[@pos] =~ /[0-7]/
                @pos += 1
              end
              buf << @src[j...@pos].to_i(8).chr
              @pos -= 1   # because main loop's @pos += 1 still applies
            else
              # unknown escape: keep literal (textproto is forgiving)
              buf << esc
            end
            @pos += 1
          elsif c == "\n"
            raise ParseError, "#{loc}: newline in string literal"
          else
            buf << c
            @pos += 1
          end
        end
        raise ParseError, "#{loc}: unterminated string" if @pos >= @len
        @pos += 1                  # consume closing quote

        # Adjacent string literals concatenate ('foo' 'bar' → 'foobar').
        # We save *both* @pos and @line because skip_ws walks newlines
        # and we'd otherwise double-count them on the rewind.
        save_pos, save_line = @pos, @line
        skip_ws
        if @pos < @len && (@src[@pos] == '"' || @src[@pos] == "'")
          quote = @src[@pos]
          # loop continues; the @pos += 1 at the top will skip the new opening quote
        else
          @pos, @line = save_pos, save_line
          return buf
        end
      end
    end

    def read_number
      start = @pos
      @pos += 1 if @src[@pos] == '-' || @src[@pos] == '+'
      @pos += 1 while @pos < @len && @src[@pos] =~ /[0-9]/
      is_float = false
      if @pos < @len && @src[@pos] == '.'
        is_float = true
        @pos += 1
        @pos += 1 while @pos < @len && @src[@pos] =~ /[0-9]/
      end
      if @pos < @len && (@src[@pos] == 'e' || @src[@pos] == 'E')
        is_float = true
        @pos += 1
        @pos += 1 if @pos < @len && (@src[@pos] == '-' || @src[@pos] == '+')
        @pos += 1 while @pos < @len && @src[@pos] =~ /[0-9]/
      end
      text = @src[start...@pos]
      # consume optional 'u'/'U' (uint marker), 'f'/'F' (float marker)
      if @pos < @len && (@src[@pos] == 'u' || @src[@pos] == 'U')
        @pos += 1
        return text.to_i
      end
      if @pos < @len && (@src[@pos] == 'f' || @src[@pos] == 'F')
        @pos += 1
        return text.to_f
      end
      is_float ? text.to_f : text.to_i
    end

    def read_ident
      start = @pos
      @pos += 1 while @pos < @len && @src[@pos] =~ /[A-Za-z_0-9.]/
      @src[start...@pos]
    end
  end

  # ---- parser --------------------------------------------------------------

  def self.parse(source, path = '<input>')
    lexer = Lexer.new(source, path)
    msg = Message.new
    parse_body(lexer, msg, top: true)
    msg
  end

  def self.parse_file(path)
    parse(File.read(path), path)
  end

  def self.parse_body(lexer, msg, top: false)
    loop do
      tok, val, loc = lexer.next_token
      case tok
      when :eof
        raise ParseError, "#{loc}: unexpected EOF (unclosed message)" unless top
        return
      when :rbrace
        raise ParseError, "#{loc}: stray '}'" if top
        return
      when :comma
        # Some textproto generators emit `field: value,` between
        # fields.  The grammar treats the comma as a separator that
        # is otherwise ignorable.  Just skip it.
        next
      when :lbracket
        # Proto extension/Any syntax: `[fully.qualified.name] { ... }`
        # or `[type.googleapis.com/cel.expr.foo.Bar] { ... }` (Any).
        # The bracketed content is a free-form identifier path that may
        # include '.', '/' and alphanumerics — we just slurp raw bytes
        # up to the next ']' rather than running it through the lexer.
        key = lexer.slurp_until_rbracket
        peek_tok, _, peek_loc = lexer.next_token
        if peek_tok == :colon
          parse_field_value(lexer, msg, key)
        elsif peek_tok == :lbrace
          submsg = Message.new
          parse_body(lexer, submsg)
          msg.add(key, submsg)
        else
          raise ParseError, "#{peek_loc}: expected ':' or '{' after extension field, got #{peek_tok}"
        end
      when :ident
        key = val
        peek_tok, peek_val, peek_loc = lexer.next_token
        if peek_tok == :colon
          parse_field_value(lexer, msg, key)
        elsif peek_tok == :lbrace
          submsg = Message.new
          parse_body(lexer, submsg)
          msg.add(key, submsg)
        else
          raise ParseError, "#{peek_loc}: expected ':' or '{' after field name '#{key}', got #{peek_tok} (#{peek_val.inspect})"
        end
      else
        raise ParseError, "#{loc}: expected field name, got #{tok} (#{val.inspect})"
      end
    end
  end

  def self.parse_field_value(lexer, msg, key)
    tok, val, loc = lexer.next_token
    case tok
    when :lbrace
      submsg = Message.new
      parse_body(lexer, submsg)
      msg.add(key, submsg)
    when :lbracket
      # repeated value list: `[v1, v2, ...]`.  Elements may be scalars
      # *or* submessages (`[ { ... }, { ... } ]` — proto2 fixtures use this
      # for repeated message fields).
      loop do
        tok, val, loc = lexer.next_token
        break if tok == :rbracket
        if tok == :lbrace
          submsg = Message.new
          parse_body(lexer, submsg)
          msg.add(key, submsg)
        else
          msg.add(key, scalar_token_to_value(tok, val, loc))
        end
        tok, val, loc = lexer.next_token
        break if tok == :rbracket
        raise ParseError, "#{loc}: expected ',' or ']' in list, got #{tok}" unless tok == :comma
      end
    when :string, :number
      msg.add(key, val)
    when :ident
      msg.add(key, ident_to_value(val))
    else
      raise ParseError, "#{loc}: expected value after '#{key}:', got #{tok} (#{val.inspect})"
    end
  end

  def self.ident_to_value(s)
    case s
    when 'true'  then true
    when 'false' then false
    else              s.to_sym       # enum or bare keyword
    end
  end

  def self.scalar_token_to_value(tok, val, loc)
    case tok
    when :string, :number then val
    when :ident           then ident_to_value(val)
    else raise ParseError, "#{loc}: expected scalar in list, got #{tok}"
    end
  end
end

# ---- self-test when run directly -----------------------------------------

if $PROGRAM_NAME == __FILE__
  require 'pp'
  paths = ARGV.empty? \
    ? Dir[File.join(__dir__, 'conformance', '*.textproto')]
    : ARGV
  paths.each do |p|
    print "#{File.basename(p)}: "
    begin
      m = TextProto.parse_file(p)
      sections = m['section']
      tests = sections.sum { |s| s['test'].size }
      puts "ok (#{sections.size} sections, #{tests} tests)"
    rescue TextProto::ParseError => e
      puts "PARSE ERROR: #{e.message}"
    end
  end
end
