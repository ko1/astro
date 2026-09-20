# UTF-8 の `.*?` は反復ノードに特殊化されておらず汎用の再帰パスに落ちていた
# (greedy には UTF-8 版があった) → 200 KB で "regexp match stack overflow"。
# さらに先頭が `.*` の pattern は開始位置を 1 つ試せば十分なので、
# for-each-start-position ループを 1 回で打ち切る。両方入れて O(n)。
# (rack の multipart parser が 1 MB のバッファに /(.*?\r\n)\r\n/m を当ててハングしていた)
require "strscan"
[/(.*?\r\n)\r\n/m, /.*?\r\n\r\n/m, /(?:.*?\r\n)\r\n/m, /(.*?)\r\n\r\n/m, /(.*\r\n)\r\n/m].each do |re|
  s = "x" * 200_000
  p [re.source, re.match(s).nil?]
end
# 見つかる側
body = ("a" * 100_000) + "\r\n\r\ntail"
m = /(.*?\r\n)\r\n/m.match(body)
p [m[0].bytesize, m[1].bytesize, m.begin(0), m.end(0)]
p body.index(/\r\n\r\n/)
sc = StringScanner.new(body)
r = sc.scan_until(/(.*?\r\n)\r\n/m)
p [r.bytesize, sc.pos, sc.rest_size]
# 先頭が .* でない pattern は今までどおり全開始位置を試す
p("hello world" =~ /world/)
p("hello world" =~ /o w/)
p("aaab".match(/a*b/)[0])
p("xxaby".match(/a.?b/)[0])
p("abc\ndef".match(/^def/)[0])
p("abc\ndef" =~ /c.d/m)
p("abc\ndef" =~ /c.d/)
# 複数行・日本語
jp = "あいう\r\n\r\nえお"
p(/(.*?\r\n)\r\n/m.match(jp)[1])
p(/.*/m.match("あい\nう")[0])
p(/.*/.match("あい\nう")[0])
