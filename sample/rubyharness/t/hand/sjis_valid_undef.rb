# Shift_JIS family: validity is byte structure; a well-formed but unmapped character
# is an undefined conversion; CP932's many-to-one rows (NEC-selected IBM ext.) decode
%w[Shift_JIS Windows-31J SJIS CP932 MacJapanese].each { |e| p [e, "\x82\x41".force_encoding(e).valid_encoding?, "\xED\x69".force_encoding(e).valid_encoding?, "\x82".force_encoding(e).valid_encoding?] }
["\x87\x40", "\xED\x40", "\xED\x69", "\xFA\x40", "\x82\xA0", "\x82\x41"].each do |b|
  p %w[Windows-31J Shift_JIS].map { |e| begin; b.dup.force_encoding(e).encode("UTF-8"); rescue => ex; [ex.class, ex.message]; end }
end
p "invalid\xFFsjis".force_encoding("sjis").encode("utf-8", invalid: :replace, undef: :replace)
p "a\x81\xC6b".force_encoding("sjis").scrub, "a\x81\xC6b".force_encoding("sjis").valid_encoding?
