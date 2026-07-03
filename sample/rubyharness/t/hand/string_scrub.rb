# String#scrub / #valid_encoding? — UTF-8 validation + invalid-sequence
# replacement. vs ruby.
p "abc".scrub
p "café".scrub
p "abc\xE3\x81".scrub
p "a\xFFb".scrub
p "a\xFFb".scrub("?")
p "\xE3\x81\x82".scrub
p "abc".valid_encoding?
p "café".valid_encoding?
p "abc\xE3\x81".valid_encoding?
p "\xFF".valid_encoding?
p "\xE3\x81\x82".valid_encoding?
p "a".scrub(true)
p "a".scrub(nil)
p "\x80\x80".scrub
