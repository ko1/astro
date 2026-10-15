# Converter: crlf convpath, ISO-2022-JP shift state across calls, read-again kept,
# UTF-16 lone surrogate, incomplete input at #finish, Big5 (2026-10-16)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
t(:convpath_crlf) { Encoding::Converter.new("ISO-8859-1", "EUC-JP", crlf_newline: true).convpath }
t(:convpath_plain) { Encoding::Converter.new("ASCII", "UTF-8", crlf_newline: false).convpath }
t(:jis_state) { ec = Encoding::Converter.new("utf-8", "iso-2022-jp"); [ec.convert("\u{9999}").b, ec.convert("\u{9999}a").b, ec.finish.b] }
t(:jis_ascii_end) { ec = Encoding::Converter.new("utf-8", "iso-2022-jp"); [ec.convert("\u{9999}a").b, ec.finish.b] }
t(:jis_encode) { "\u{9999}".encode("iso-2022-jp").b }
t(:read_again) do
  ec = Encoding::Converter.new("utf-8", "euc-jp"); s = +"\xf1abcd"; d = +""
  [ec.primitive_convert(s, d), s, ec.primitive_errinfo, ec.primitive_convert(s, d), d]
end
t(:putback_drops) do
  ec = Encoding::Converter.new("utf-8", "euc-jp"); s = +"\xf1abcd"; d = +""
  ec.primitive_convert(s, d); pb = ec.putback; [pb, ec.primitive_convert(s, d), d]
end
t(:utf16_lone) do
  ec = Encoding::Converter.new("utf-16le", "utf-8"); src = "\x00\xd8\x61\x00".b; d = +""
  [ec.primitive_convert(src, d), ec.primitive_errinfo, ec.putback.b]
end
t(:utf16_replace) { "\x00\xd8\x61\x00".b.force_encoding("UTF-16LE").encode("UTF-8", invalid: :replace) }
t(:finish_incomplete) do
  ec = Encoding::Converter.new("utf-8", "euc-jp")
  ec.primitive_convert("\xE3".b.force_encoding("utf-8"), +"", nil, nil, partial_input: true)
  r = (ec.finish rescue $!); [r.class, r.message, ec.primitive_errinfo]
end
t(:big5) { "\u{4E00}".encode("Big5").b }
