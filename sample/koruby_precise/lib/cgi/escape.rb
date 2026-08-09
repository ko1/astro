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
  def self.escape(s)
    s.to_s.gsub(/[^A-Za-z0-9_.\-~ ]/) { |c| c.bytes.map { |b| "%%%02X" % b }.join }.tr(" ", "+")
  end
  def self.unescape(s)
    s.to_s.tr("+", " ").gsub(/%([0-9A-Fa-f]{2})/) { [$1.to_i(16)].pack("C") }
  end
  def self.escapeURIComponent(s)
    s.to_s.gsub(/[^A-Za-z0-9_.\-~]/) { |c| c.bytes.map { |b| "%%%02X" % b }.join }
  end
  def self.unescapeURIComponent(s)
    s.to_s.gsub(/%([0-9A-Fa-f]{2})/) { [$1.to_i(16)].pack("C") }
  end
  class << self
    alias escape_html escapeHTML
    alias unescape_html unescapeHTML
    alias escape_uri_component escapeURIComponent
    alias unescape_uri_component unescapeURIComponent
  end
end
