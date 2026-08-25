# 最小 CGI escape 群 (pure Ruby)。本体 cgi.rb (フォーム処理等) は未対応。
module CGI
  HTML_ESCAPE = { "&" => "&amp;", '"' => "&quot;", "<" => "&lt;", ">" => "&gt;", "'" => "&#39;" }
  HTML_UNESCAPE = HTML_ESCAPE.invert
  def self.escapeHTML(s)
    s.to_s.gsub(/['&"<>]/) { |c| HTML_ESCAPE[c] }
  end
  def self.unescapeHTML(s)
    s.to_s.gsub(/&(amp|quot|lt|gt|#39|apos);/) do |m|
      { "&amp;" => "&", "&quot;" => '"', "&lt;" => "<", "&gt;" => ">", "&#39;" => "'", "&apos;" => "'" }[m]
    end
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
    s.to_s.gsub(/[^A-Za-z0-9_.\-~ ]/) { |c| c.bytes.map { |b| "%%%02X" % b }.join }.tr(" ", "+")
  end
  def self.unescape(s, encoding = nil)
    enc = __decode_enc(encoding)
    __decoded(__str(s).tr("+", " "), enc)
  end
  def self.escapeURIComponent(s)
    s.to_s.gsub(/[^A-Za-z0-9_.\-~]/) { |c| c.bytes.map { |b| "%%%02X" % b }.join }
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
    out = str.gsub(/%([0-9A-Fa-f]{2})/) { [$1.to_i(16)].pack("C") }
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
