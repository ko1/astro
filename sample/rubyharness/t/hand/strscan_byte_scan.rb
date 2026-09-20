# StringScanner は byte カーソルを持つので、Regexp#__match_byte で
# そのまま照合する (文字インデックスに直すと走査のたびに O(n) かかり、
# 大きな入力の走査が O(n^2) になっていた)。多バイトでも同じ答えになること。
require "strscan"
s = StringScanner.new("aéb cé d")
p s.scan_until(/é/), s.pos, s.charpos
p s.scan(/b/), s.pos, s.charpos
p s.scan_until(/c(é)/), s[1], s.pos, s.charpos
p s.rest, s.matched, s.pre_match.nil? ? nil : true
t = StringScanner.new("x" * 50 + "END" + "y" * 50)
p t.scan_until(/END/).size, t.pos, t.rest_size
u = StringScanner.new("aaa")
p u.scan(/\Aa/), u.pos
p u.scan(/\Aa/), u.pos
p u.check(/a/), u.match?(/a/), u.skip(/a/), u.pos
# 大きな入力の走査が線形であること (以前は O(n^2) で 200k 文字に 9 秒かかった)
big = StringScanner.new("abcdefghij" * 20_000)
n = 0
n += 1 while big.scan_until(/e/)
p n, big.eos?
