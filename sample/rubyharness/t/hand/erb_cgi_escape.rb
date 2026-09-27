require 'erb'

p ERB::Escape.method(:html_escape).source_location
p ERB::Util.method(:html_escape).source_location
p ERB::Util.h("<&>\"'x"), ERB::Util.html_escape(1), ERB::Util.html_escape(nil)
s = "plain"
p ERB::Util.html_escape(s).equal?(s), CGI.escapeHTML(s).equal?(s)
k = Class.new(String) { def to_s = "<to_s>" }
p ERB::Util.html_escape(k.new("<b>")), CGI.escapeHTML(k.new("<b>"))
o = Object.new
def o.to_str = "<a>"
p CGI.escapeHTML(o)
p((CGI.escapeHTML(1) rescue $!.class))
p CGI.escapeHTML("<あ>".encode("UTF-16LE")).encode("UTF-8")
p ERB::Util.html_escape("<あ>".encode("UTF-16LE")).encode("UTF-8")
e = CGI.escapeHTML("a<\xA5\xB5".dup.force_encoding("EUC-JP"))
p e.encoding, e.b
p CGI.class
p ERB::Util.url_encode("\xA5\xB5\xA5\xF3".dup.force_encoding("EUC-JP"))
p CGI.escape("\xA5\xB5 a".dup.force_encoding("EUC-JP"))
