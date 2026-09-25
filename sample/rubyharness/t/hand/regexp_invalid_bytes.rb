# A subject whose bytes are not valid in its own encoding is an ArgumentError
# for every match form, not just sub/gsub/split/scan (CRuby buckets the match by
# the subject's coderange first).  rubocop's PathUtil feeds exactly this.
def t(l)
  p [l, yield]
rescue ArgumentError => e
  p [l, e.class, e.message]
end

s = "dir/file\xBF"
p s.valid_encoding?, s.encoding.to_s

t(:re_match_p)  { /^d.*$/.match?(s) }
t(:re_match)    { /d/.match(s) }
t(:re_eqtilde)  { /d/ =~ s }
t(:re_caseeq)   { /d/ === s }
t(:str_eqtilde) { s =~ /d/ }
t(:str_match)   { s.match(/d/) }
t(:str_match_p) { s.match?(/d/) }
t(:gsub)        { s.gsub(/d/, "x") }
t(:sub)         { s.sub(/d/, "x") }
t(:split)       { s.split(/\//) }
t(:scan)        { s.scan(/d/) }
t(:index)       { s.index(/d/) }
t(:grep)        { [s].grep(/d/) }
t(:case_when)   { case s when /d/ then :hit else :miss end }

# valid strings are untouched
ok = "dir/file"
t(:ok_match_p)  { /^d.*$/.match?(ok) }
t(:ok_eqtilde)  { ok =~ /file/ }
t(:ok_match)    { ok.match(/(f)(i)/).captures }
t(:ok_gsub)     { ok.gsub(/i/, "I") }
t(:multibyte)   { "ディレクトリ/名前".match(/名前/)[0] }
t(:mb_match_p)  { "ディレクトリ".match?(/レク/) }

# BINARY / US-ASCII subjects: every byte is legal in BINARY
b = "dir/file\xBF".dup.force_encoding("ASCII-8BIT")
t(:binary)      { b.valid_encoding? }
t(:binary_m)    { /d/.match?(b) }
u = "dir\xBF".dup.force_encoding("US-ASCII")
t(:usascii_v)   { u.valid_encoding? }

# a pos argument does not skip the check
t(:with_pos)    { /d/.match(s, 2) }
t(:mq_with_pos) { /d/.match?(s, 2) }
