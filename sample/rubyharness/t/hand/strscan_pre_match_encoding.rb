# StringScanner#pre_match runs from the start of the string, also for patterns with ^ / \A.
require 'strscan'
s = StringScanner.new("ab\tcd"); s.pos = 2; s.scan_until(/(^[ \t]*)?c/); p s.pre_match, s.post_match, s.matched
s = StringScanner.new("xyz"); s.pos = 1; s.scan(/y/); p s.pre_match; s.scan(/^z/); p s.pre_match
s = StringScanner.new("a\nb"); s.pos = 2; p s.scan(/^b/), s.pre_match, s.post_match
s = StringScanner.new("héllo wörld"); s.scan(/h.l/); s.scan_until(/\Alo w/); p s.pre_match, s.pre_match.encoding
s = StringScanner.new("abc"); s.getch; p s.pre_match; s.check(/^b/); p s.pre_match; s.skip(/\Ab/); p s.pre_match
# A Regexp scan on a broken string raises (a String pattern does not).
t = ->(&b) { b.call rescue "#{$!.class}: #{$!.message}" }
p [t.() { StringScanner.new("\xff").scan(/a/) }, t.() { StringScanner.new("a\xff").scan(/a/) }, t.() { StringScanner.new("\xff").scan("a") },
   t.() { StringScanner.new("\xff".b).scan(/a/) }, t.() { StringScanner.new("\xffa").scan_until(/a/) }, t.() { "\xff" =~ /a/ }, t.() { "\xff".match?(/a/) },
   t.() { StringScanner.new("a\xff").scan(/a/) && :ok }, t.() { s = StringScanner.new("ab\xff"); s.scan(/a/); s.scan(/b/) }]
