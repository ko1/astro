# A character well-formed in its encoding but with no Unicode mapping is an
# undefined conversion (one replacement), not invalid bytes.
s = (+"# \x8E\xE0\n").force_encoding("EUC-JP")
p s.encode("UTF-8", invalid: :replace, undef: :replace)
begin; s.encode("UTF-8"); rescue => e; p e.class, e.message, e.error_char.bytes; end
p "\xA4\xA2".force_encoding("EUC-JP").encode("UTF-8"), (+"a\xA1").force_encoding("EUC-JP").encode("UTF-8", invalid: :replace)
