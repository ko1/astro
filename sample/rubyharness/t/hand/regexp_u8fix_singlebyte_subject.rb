# A pattern CRuby fixes to UTF-8 (a \u escape >= 0x80, \p, a non-ASCII UTF-8
# source, /u) still matches an ASCII-only US-ASCII / BINARY / Latin-1 subject in
# its own encoding; a non-ASCII subject in another encoding is a
# CompatibilityError.  (rexml's entity / XPath regexps against Integer#to_s.)
cls = "[a-z\\u00C0-\\u00D6]"
re = /#{cls}/
ascii, bin, lat = "abc".encode("US-ASCII"), "abc".b, "abc".encode("ISO-8859-1")
p re.encoding, re.fixed_encoding?, re.options & Regexp::FIXEDENCODING
p re =~ ascii, re =~ 5.to_s, re =~ bin, re =~ lat, re === ascii, "x5y".gsub(/[À-Ö\d]/, "-")
p bin.scan(re), ascii.sub(re, "Z"), [ascii, bin].grep(re), (case bin when re then :hit end)
p (/[Àa]/u =~ ascii), (/[\u{C0 C1}a]/ =~ ascii), (Regexp.new("[\\u00C0-\\u00D6a]".b) =~ bin)
p (Regexp.union(re, /b/) =~ bin), (/[À-Öa]/ =~ ascii), (/\p{Alpha}/ =~ ascii), /\p{Alpha}/.options
p((re =~ "\xC3\x80".b rescue $!.class), (/[Àx]/ === "\xC3\x80".b rescue $!.class))
p /\\u00C0/.encoding, /\\u00C0/.fixed_encoding?, /A/.encoding, /A/ =~ bin.upcase
p re == /#{cls}/, re.hash == /#{cls}/.hash, (re =~ ascii; re) == /#{cls}/, $~ == (/#{cls}/ =~ ascii; $~)
p "5".match?(re), bin.index(re), bin.split(re), ascii.start_with?(re)
