# gsub(regexp, hash) のキーはマッチ部分文字列で、CRuby では **subject の
# encoding** を引き継ぐ。koruby は既定 encoding で作っていたので、binary の
# 高位バイトが binary なテーブルに当たらず空文字になっていた。
# (URI.encode_www_form_component がこの形のテーブル gsub。rack の
#  Rack::Utils.escape が全滅していた)
tbl = {}
256.times { |i| tbl[-i.chr] = -('%%%02X' % i) }
tbl[' '] = '+'
r = /[^*\-.0-9A-Z_a-z]/

s = "あ".dup.force_encoding(Encoding::ASCII_8BIT)
p s.bytes
p s.gsub(r, tbl)
t = s.dup
t.gsub!(r, tbl)
p t
p "a b".dup.force_encoding("ASCII-8BIT").gsub(r, tbl)
p "abc".gsub(/[ab]/, { "a" => "A" })
p "ø".encode("ISO-8859-1").b.gsub(r, tbl)

require "uri"
p URI.encode_www_form_component("まつもと")
p URI.encode_www_form_component("a b")
p URI.encode_www_form_component("ø".encode("ISO-8859-1"))
p URI.encode_www_form_component("abc")
p URI.decode_www_form_component("%E3%81%BE")

# UTF-8 subject のときのキー encoding
seen = nil
"あx".gsub(/./) { |m| seen ||= m.encoding.to_s; m }
p seen
h = { "あ" => "OK" }
p "あx".gsub(/./, h)
