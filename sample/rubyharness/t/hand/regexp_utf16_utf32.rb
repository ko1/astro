# Regexps in UTF-16LE/BE and UTF-32LE/BE match strings of the same encoding (via a
# UTF-8 decode with an offset map): Regexp.new / escape / union, match, scan, split,
# gsub, StringScanner; offsets and captures as CRuby.  csv UTF-16 (w12 A10).
require 'strscan'
def t(label)
  r = begin; yield; rescue Exception => e; "#{e.class}: #{e.message}"; end
  puts "#{label}: #{r.inspect}"
end
%w[UTF-16LE UTF-16BE UTF-32LE UTF-32BE].each do |en|
  E = Encoding.find(en) unless defined?(E)
  enc = ->(s) { s.encode(en) }
  puts "=== #{en}"
  s = enc["xa,b\u{1F600}c,d"]
  re = Regexp.new(enc[","])
  t("source") { [re.source.encoding, re.source.bytesize, re.encoding, re.fixed_encoding?] }
  t("to_s enc") { re.to_s.encoding }
  t("escape") { e = Regexp.escape(enc["a.b*"]); [e.encoding, e.encode("UTF-8")] }
  t("escape newline") { Regexp.escape(enc["\r\n\t "]).encode("UTF-8") }
  t("match?") { re.match?(s) }
  t("=~") { [s =~ re, $~ && $~.begin(0), $~ && $~.byteoffset(0)] }
  t("match") { m = re.match(s); [m.begin(0), m.pre_match.encode("UTF-8"), m.post_match.encode("UTF-8"), m[0].encoding] }
  t("match pos") { m = re.match(s, 5); [m.begin(0), m.byteoffset(0)] }
  t("captures") { m = Regexp.new(enc["(\\w)(,)(.)"]).match(s); [m.captures.map { _1.encode("UTF-8") }, m.offset(3), m.byteoffset(3)] }
  t("split") { s.split(re).map { _1.encode("UTF-8") } }
  t("split -1") { enc["a,,b,"].split(re, -1).map { _1.encode("UTF-8") } }
  t("scan") { s.scan(Regexp.new(enc["\\w"])).map { _1.encode("UTF-8") } }
  t("gsub") { s.gsub(re, enc[";"]).encode("UTF-8") }
  t("gsub! str") { x = s.dup; x.gsub!(re, enc["<>"]); x.encode("UTF-8") }
  t("sub") { s.sub(re, enc[";"]).encode("UTF-8") }
  t("index") { [s.index(re), s.index(re, 5), s.rindex(re)] }
  t("byteindex") { s.byteindex(re) }
  t("=== ") { re === s }
  t("case") { case s; when re then $~.begin(0); end }
  t("dot astral") { Regexp.new(enc["b(.)c"]).match(s)[1].encode("UTF-8") }
  t("anchor \\z") { Regexp.new(enc["d\\z"]).match?(s) }
  t("\\A") { Regexp.new(enc["\\Ax"]).match?(s) }
  t("union") { u = Regexp.union(enc["a"], enc["."]); [u.encoding, u.source.encode("UTF-8"), u.match?(s)] }
  t("union re") { u = Regexp.union(re, Regexp.new(enc[";"])); [u.encoding, u.match?(s)] }
  t("utf8 re on u16") { /,/.match?(s) }
  t("u16 re on utf8") { re.match?("a,b") }
  t("u16 re on ascii") { re.match?("a,b".b) }
  t("[^\\r\\n]+") { Regexp.new(enc["[^\r\n]+"]).match(enc["ab\r\ncd"])[0].encode("UTF-8") }
  t("alternation") { Regexp.new(enc["\r\n|\n|\r"]).match(enc["ab\r\ncd"]).byteoffset(0) }
  t("ignorecase") { Regexp.new(enc["A"], Regexp::IGNORECASE).match?(enc["xa"]) }
  t("invalid subject") { re.match?(s.byteslice(0, s.bytesize - 1)) }
  t("strscan scan") { ss = StringScanner.new(s); [ss.scan(Regexp.new(enc["x"]))&.encode("UTF-8"), ss.pos] }
  t("strscan scan_until") { ss = StringScanner.new(s); [ss.scan_until(re)&.encode("UTF-8"), ss.pos, ss.pre_match&.encode("UTF-8"), ss.matched&.encode("UTF-8")] }
  t("strscan skip") { ss = StringScanner.new(s); [ss.skip(Regexp.new(enc["xa"])), ss.pos, ss.check(re), ss.skip_until(re), ss.pos] }
  t("strscan [] ") { ss = StringScanner.new(s); ss.scan(Regexp.new(enc["(x)(a)"])); [ss[1].encode("UTF-8"), ss[2].encoding] }
end
