# CRuby's String#inspect renders into Encoding.default_external: a string whose
# encoding is NOT that one comes out with escapes (rb_str_inspect's
# `enc != resenc` path).  koruby only looked at the string's own encoding, so a
# UTF-8 string printed its bytes verbatim under a US-ASCII external — rubocop's
# Style/WordArray builds `%W(ВУЗ)` out of exactly this.
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 50]]
end

def with_ext(e)
  orig = Encoding.default_external
  Encoding.default_external = e
  yield
ensure
  Encoding.default_external = orig
end

U = "ВУЗ中文"

t(:utf8_default) { with_ext(Encoding::UTF_8) { U.inspect } }
t(:usascii)      { with_ext(Encoding::US_ASCII) { U.inspect } }
t(:binary_ext)   { with_ext(Encoding::BINARY) { U.inspect } }
t(:eucjp_ext)    { with_ext(Encoding::EUC_JP) { U.inspect } }
t(:ascii_str)    { with_ext(Encoding::US_ASCII) { "abc".inspect } }
t(:escapes)      { with_ext(Encoding::US_ASCII) { "a\tb\"c\\d".inspect } }
t(:emoji)        { with_ext(Encoding::US_ASCII) { "a\u{1F600}b".inspect } }
t(:sym)          { with_ext(Encoding::US_ASCII) { :"ВУЗ".inspect } }
t(:sym_ascii)    { with_ext(Encoding::US_ASCII) { :abc.inspect } }
t(:array)        { with_ext(Encoding::US_ASCII) { [U].inspect } }
t(:hash)         { with_ext(Encoding::US_ASCII) { { U => U }.inspect } }
t(:restored)     { with_ext(Encoding::US_ASCII) { nil }; U.inspect }
t(:nested)       { with_ext(Encoding::US_ASCII) { with_ext(Encoding::UTF_8) { U.inspect } } }
t(:dump_same)    { U.dump }
t(:to_s_untouched) { with_ext(Encoding::US_ASCII) { U.to_s.bytesize } }
t(:enc_unchanged)  { with_ext(Encoding::US_ASCII) { U.encoding.to_s } }
t(:default_int)  { with_ext(Encoding::US_ASCII) { Encoding.default_internal.inspect } }
