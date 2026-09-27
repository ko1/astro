# StringScanner#scan / check / skip / match? try only the current position (a
# \G-anchored copy of the pattern), so a failing scan is O(1) in the rest; the
# results, captures and encodings are as before.
require 'strscan'
s = StringScanner.new("\t" * 50_000 + "a")
t = Process.clock_gettime(Process::CLOCK_MONOTONIC)
2000.times { s.scan(/\s+b/) }
p s.scan(/\s+b/), (Process.clock_gettime(Process::CLOCK_MONOTONIC) - t) < 1.0
s = StringScanner.new("foo 123 bar")
p s.scan(/\w+/), s.scan(/\d+/), s.scan(/\s+/), s.scan(/(?<num>\d+)/), s[:num], s[1], s.pos
p s.scan(/ (?# comment ) \s* b a r # trailing
      /x), s.eos?
s = StringScanner.new("aXbXc")
p s.scan_until(/X/), s.check(/b/), s.skip(/b/), s.match?(/X/), s.getch, s.scan(/c|d/), s.scan(/x|/)
u = StringScanner.new("日本語テキスト")
p u.scan(/日本/), u.scan(/\p{Han}+/), u.scan(/[ァ-ン]+/), u.pos, u.charpos
b = StringScanner.new("\xFF\x00abc".b)
p b.scan(/\xFF/n), b.scan(/\x00/n), b.scan(/abc/), b.matched.encoding
a = StringScanner.new("ab\ncd")
p a.scan(/ab$/), a.scan(/\n^cd/), a.scan(/\A/), StringScanner.new("xy", fixed_anchor: true).tap { _1.pos = 1 }.scan(/\Ay/)
p StringScanner.new("abcabc").then { |x| [x.scan(/abc/), x.scan(/abc/), x.scan(/abc/)] }
p StringScanner.new("a|b").scan(/a\|/), StringScanner.new("ab").scan(/a(?=b)/), StringScanner.new("ab").scan(/(?<=a)b/)
s = StringScanner.new("ab\ncd"); p s.scan(/[^\n]*/), s.scan(/\n^c/), s.scan(/[^x]$/)
s = StringScanner.new(" <!ATTLIST a b CDATA #IMPLIED>"); p s.scan(/\s*[^>]*>/)
s = StringScanner.new("a" * 2000 + ">b"); p s.scan(/[^>b]*b/), s.scan(/[^>]*>/).size, s.scan(/[^x]/), s.eos?
