# 最小 CGI escape 群 (pure Ruby)。本体 cgi.rb (フォーム処理等) は未対応。
module CGI
  HTML_ESCAPE = { "&" => "&amp;", '"' => "&quot;", "<" => "&lt;", ">" => "&gt;", "'" => "&#39;" }
  HTML_UNESCAPE = HTML_ESCAPE.invert
  # CRuby's C escapes work on bytes, so a string that is invalid in its own
  # encoding is still escaped: run the regexp over a BINARY copy then retag.
  def self.__bytewise(str)
    return yield(str) if str.valid_encoding?
    yield(str.b).force_encoding(str.encoding)
  end
  private_class_method :__bytewise
  def self.escapeHTML(s)
    __bytewise(s.to_s) { |str| str.gsub(/['&"<>]/) { |c| HTML_ESCAPE[c] } }
  end
  def self.unescapeHTML(s)
    __bytewise(__str(s)) { |str| str.gsub(/&(?:amp|quot|lt|gt|apos|\#[0-9]+|\#[xX][0-9A-Fa-f]+);/) do |m|
      case m
      when "&amp;"  then "&"
      when "&quot;" then '"'
      when "&lt;"   then "<"
      when "&gt;"   then ">"
      when "&apos;" then "'"
      else
        body = m[1..-2]                       # numeric character reference
        cp = body.start_with?("#x", "#X") ? body[2..-1].to_i(16) : body[1..-1].to_i
        (cp <= 0 || cp > 0x10FFFF) ? m : cp.chr(Encoding::UTF_8)
      end
    end }
  end
  # escapeElement("<A><B>", "A") — escape only the listed tags.
  def self.escapeElement(string, *elements)
    elements = elements[0] if elements[0].is_a?(Array)
    string.to_s.gsub(/<\/?(?:#{elements.join("|")})(?!\w)(?:.|\n)*?>/i) { escapeHTML($&) }
  end
  def self.unescapeElement(string, *elements)
    elements = elements[0] if elements[0].is_a?(Array)
    string.to_s.gsub(/&lt;\/?(?:#{elements.join("|")})(?!\w)(?:.|\n)*?&gt;/i) { unescapeHTML($&) }
  end
  # The target encoding of a decode: an Encoding, an encoding name, or (by
  # default) CGI.accept_charset.  An unknown name is an ArgumentError.
  @@accept_charset = Encoding::UTF_8
  def self.accept_charset; @@accept_charset; end
  def self.accept_charset=(e)          # a setter cannot be an endless def
    @@accept_charset = e
  end
  def self.__decode_enc(e)
    e = @@accept_charset if e.nil?
    e.is_a?(Encoding) ? e : Encoding.find(e.to_s)
  end
  private_class_method :__decode_enc

  def self.escape(s)
    __bytewise(__str(s)) { |str| str.gsub(/[^A-Za-z0-9_.\-~ ]/) { |c| c.bytes.map { |b| "%%%02X" % b }.join } }.tr(" ", "+")
  end
  def self.unescape(s, encoding = nil)
    enc = __decode_enc(encoding)
    __decoded(__str(s).tr("+", " "), enc)
  end
  def self.escapeURIComponent(s)
    __bytewise(__str(s)) { |str| str.gsub(/[^A-Za-z0-9_.\-~]/) { |c| c.bytes.map { |b| "%%%02X" % b }.join } }
  end
  def self.unescapeURIComponent(s, encoding = nil)
    __decoded(__str(s), __decode_enc(encoding))
  end
  # #to_str conversion, like every CGI escape helper does (nil is a TypeError).
  def self.__str(s)
    return s if s.is_a?(String)
    unless s.respond_to?(:to_str)
      raise TypeError, "no implicit conversion of #{s.nil? ? 'nil' : s.class} into String"
    end
    r = s.to_str
    raise TypeError, "no implicit conversion into String" unless r.is_a?(String)
    r
  end
  private_class_method :__str
  # The result carries the target encoding, unless the decoded octets are not
  # valid there — then CRuby keeps the SOURCE string's encoding.
  def self.__decoded(str, enc)
    src_enc = str.encoding
    out = str.b.gsub(/%([0-9A-Fa-f]{2})/) { [$1.to_i(16)].pack("C") }   # bytewise: the source may be invalid
    out.force_encoding(enc.name)
    out.force_encoding(src_enc.name) unless out.valid_encoding?
    out
  end
  private_class_method :__decoded
  class << self
    alias escape_html escapeHTML
    alias unescape_html unescapeHTML
    alias escape_uri_component escapeURIComponent
    alias unescape_uri_component unescapeURIComponent
  end
end
