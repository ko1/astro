# 文字列リテラルの encoding は「ファイルの encoding」だけでは決まらない。
# prism が literal ごとに forced-encoding のフラグを立てる:
#   \uNNNN が U+7F 超のバイトを出す      → UTF-8
#   \xNN が 0x7F 超 かつ ソースが US-ASCII → ASCII-8BIT
# (base64 の test は `# coding: US-ASCII` で "\377" を書くので、これが無いと
#  "\377" == "\xFF".b が false になる)
#
# あわせて、両辺とも 7-bit のときの String#+ の結果 encoding。
# CRuby は rb_enc_compatible で「右辺が 7-bit なら左辺の encoding」を返す。
# koruby は US-ASCII だけ右辺に譲っていた。

# --- 7-bit 同士の結合は左辺の encoding を保つ ------------------------------
combos = [["us-ascii", "utf-8"], ["utf-8", "us-ascii"],
          ["us-ascii", "binary"], ["binary", "us-ascii"],
          ["utf-8", "binary"], ["binary", "utf-8"]]
combos.each do |a, b|
  x = "a".dup.force_encoding(a)
  y = "b".dup.force_encoding(b)
  puts "#{a} + #{b} = #{(x + y).encoding}"
end
# 空の左辺は「右辺が 7-bit なら左辺の encoding」
combos.each do |a, b|
  x = "".dup.force_encoding(a)
  y = "b".dup.force_encoding(b)
  puts "'' #{a} + #{b} = #{(x + y).encoding}"
end

# --- eval したソースの magic comment もリテラルに効く ------------------------
['"abc"', "'abc'", '"a#{"b"}c"', '"a#{1}c"'].each do |src|
  p eval(src.dup.force_encoding("us-ascii")).encoding.to_s
end
p eval("# coding: US-ASCII \n 'abc'").encoding.to_s

# --- UTF-8 ソースでは \xNN でも UTF-8 のまま ------------------------------
p "\xFF".encoding.to_s
p "é".encoding.to_s
p "a#{1}c".encoding.to_s

# --- strict base64 は余りビットが 0 でないと ArgumentError ------------------
require "base64"
["AB==", "AAB=", "AA==", "AAA=", "/w=="].each do |s|
  begin
    p Base64.strict_decode64(s).bytes
  rescue ArgumentError => e
    p [s, e.class.to_s, e.message]
  end
end
p Base64.decode64("AB==").bytes      # 寛容版は通す
