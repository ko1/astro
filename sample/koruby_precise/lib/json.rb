# frozen_string_literal: true
#
# json.rb — a pure-Ruby JSON parser + generator for koruby_precise.
#
# koruby ships no C json extension; this provides the common surface:
#   JSON.parse(str, symbolize_names: false)  → Ruby objects
#   JSON.generate(obj) / obj.to_json         → JSON text
#   JSON.pretty_generate(obj)                → indented JSON text
#   JSON.dump / JSON.load                    → aliases
# Conforms to RFC 8259 for the value grammar (objects, arrays, strings with
# \uXXXX + surrogate pairs, numbers, true/false/null).

module JSON
  VERSION = "2.20.0"
  class ParserError < StandardError; end
  class GeneratorError < StandardError; end

  # ---- parsing -------------------------------------------------------------
  class Parser
    def initialize(source, symbolize_names: false)
      @s   = source.to_str
      @i   = 0
      @len = @s.length
      @sym = symbolize_names
    end

    def parse
      skip_ws
      v = parse_value
      skip_ws
      err("unexpected trailing character") if @i < @len
      v
    end

    private

    def err(msg)
      raise ParserError, "#{msg} at position #{@i}"
    end

    def skip_ws
      @i += 1 while @i < @len && (c = @s[@i]) && (c == " " || c == "\t" || c == "\n" || c == "\r")
    end

    def parse_value
      c = @s[@i]
      err("unexpected end of input") if c.nil?
      case c
      when "{" then parse_object
      when "[" then parse_array
      when '"' then parse_string
      when "t" then lit("true", true)
      when "f" then lit("false", false)
      when "n" then lit("null", nil)
      else
        if c == "-" || (c >= "0" && c <= "9")
          parse_number
        else
          err("unexpected character #{c.inspect}")
        end
      end
    end

    def lit(word, val)
      if @s[@i, word.length] == word
        @i += word.length
        val
      else
        err("invalid literal")
      end
    end

    def parse_object
      @i += 1 # {
      obj = {}
      skip_ws
      if @s[@i] == "}"
        @i += 1
        return obj
      end
      loop do
        skip_ws
        err("expected string key") unless @s[@i] == '"'
        key = parse_string
        key = key.to_sym if @sym
        skip_ws
        err("expected ':'") unless @s[@i] == ":"
        @i += 1
        skip_ws
        obj[key] = parse_value
        skip_ws
        case @s[@i]
        when "," then @i += 1
        when "}" then @i += 1; break
        else err("expected ',' or '}'")
        end
      end
      obj
    end

    def parse_array
      @i += 1 # [
      arr = []
      skip_ws
      if @s[@i] == "]"
        @i += 1
        return arr
      end
      loop do
        skip_ws
        arr << parse_value
        skip_ws
        case @s[@i]
        when "," then @i += 1
        when "]" then @i += 1; break
        else err("expected ',' or ']'")
        end
      end
      arr
    end

    def parse_string
      @i += 1 # opening "
      out = +""
      while @i < @len
        c = @s[@i]
        if c == '"'
          @i += 1
          return out
        elsif c == "\\"
          @i += 1
          e = @s[@i]
          case e
          when '"'  then out << '"'
          when "\\" then out << "\\"
          when "/"  then out << "/"
          when "b"  then out << "\b"
          when "f"  then out << "\f"
          when "n"  then out << "\n"
          when "r"  then out << "\r"
          when "t"  then out << "\t"
          when "u"  then out << parse_unicode
          else err("invalid escape \\#{e}")
          end
          @i += 1 unless e == "u" # parse_unicode already advanced past the hex
        else
          out << c
          @i += 1
        end
      end
      err("unterminated string")
    end

    def hex4
      h = @s[@i + 1, 4]
      err("invalid \\u escape") unless h && h.length == 4
      @i += 4
      h.to_i(16)
    end

    def parse_unicode
      cp = hex4
      if cp >= 0xD800 && cp <= 0xDBFF          # high surrogate → expect a low surrogate
        if @s[@i + 1] == "\\" && @s[@i + 2] == "u"
          @i += 2
          lo = hex4
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00)
        end
      end
      cp.chr(Encoding::UTF_8)
    end

    def parse_number
      start = @i
      @i += 1 if @s[@i] == "-"
      @i += 1 while @i < @len && (d = @s[@i]) && d >= "0" && d <= "9"
      is_float = false
      if @s[@i] == "."
        is_float = true
        @i += 1
        @i += 1 while @i < @len && (d = @s[@i]) && d >= "0" && d <= "9"
      end
      if (e = @s[@i]) && (e == "e" || e == "E")
        is_float = true
        @i += 1
        @i += 1 if (sg = @s[@i]) && (sg == "+" || sg == "-")
        @i += 1 while @i < @len && (d = @s[@i]) && d >= "0" && d <= "9"
      end
      tok = @s[start...@i]
      is_float ? tok.to_f : tok.to_i
    end
  end

  # ---- generation ----------------------------------------------------------
  ESCAPE_MAP = {
    '"'  => '\\"', "\\" => "\\\\", "\b" => '\\b', "\f" => '\\f',
    "\n" => '\\n', "\r" => '\\r', "\t" => '\\t'
  }.freeze

  def self.escape(str)
    out = +'"'
    str.each_char do |c|
      if (m = ESCAPE_MAP[c])
        out << m
      elsif c < " "
        out << format('\\u%04x', c.ord)
      else
        out << c
      end
    end
    out << '"'
    out
  end

  def self.gen(obj, indent, cur)
    case obj
    when nil     then "null"
    when true    then "true"
    when false   then "false"
    when String  then escape(obj)
    when Integer then obj.to_s
    when Float   then obj.to_s
    when Symbol  then escape(obj.to_s)
    when Array
      return "[]" if obj.empty?
      if indent.empty?
        "[" + obj.map { |e| gen(e, indent, cur) }.join(",") + "]"
      else
        nxt = cur + indent
        "[\n" + obj.map { |e| nxt + gen(e, indent, nxt) }.join(",\n") + "\n" + cur + "]"
      end
    when Hash
      return "{}" if obj.empty?
      if indent.empty?
        "{" + obj.map { |k, v| escape(k.to_s) + ":" + gen(v, indent, cur) }.join(",") + "}"
      else
        nxt = cur + indent
        "{\n" + obj.map { |k, v| nxt + escape(k.to_s) + ": " + gen(v, indent, nxt) }.join(",\n") + "\n" + cur + "}"
      end
    else
      if obj.respond_to?(:to_json)
        obj.to_json
      else
        escape(obj.to_s)
      end
    end
  end

  def self.parse(source, symbolize_names: false, **_opts)
    Parser.new(source, symbolize_names: symbolize_names).parse
  end

  def self.generate(obj, *_)
    gen(obj, "", "")
  end

  def self.dump(obj, *_)
    generate(obj)
  end

  def self.load(source, *_)
    parse(source)
  end

  def self.pretty_generate(obj, *_)
    gen(obj, "  ", "")
  end
end

# Object#to_json + core-type conveniences (delegate to JSON.generate).
class Object
  def to_json(*) = JSON.generate(self)
end
