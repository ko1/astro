# valid_encoding? / scrub / encode(invalid: :replace) judge the byte structure the
# way CRuby (Onigmo) does, per encoding: EUC-JP SS2/SS3, Emacs-Mule, GB18030.
def v(b, e) = b.b.force_encoding(e).valid_encoding?
p v("\x8E\xE0", "EUC-JP"), v("\x8E\xA1", "EUC-JP"), v("\x8E\xDF", "EUC-JP"), v("\x8E\x80", "EUC-JP"), v("\x8F\xA1\xA1", "EUC-JP"), v("\xA1", "EUC-JP")
p v("\x80", "Emacs-Mule"), v("\x81\xA1", "Emacs-Mule"), v("\x92\xA1\xA1", "Emacs-Mule"), v("\xA1", "Emacs-Mule"), v("a", "Emacs-Mule")
p v("\xFF", "GB18030"), v("\x81\x30\x81\x30", "GB18030"), v("\x81\x40", "GB18030"), v("\x81", "GB18030"), v("\x80", "GB18030"), v("\x81\x30", "GB18030")
p v("\x8E\xE0", "CP51932"), v("\x8E\xE0", "eucJP-ms")
t = "\x80".b.force_encoding(Encoding::Emacs_Mule)
p t.valid_encoding?, t.scrub("?").bytes, t.scrub.bytes
e = "a\xA1b\x8F\xA1c\xA4\xA2".b.force_encoding("EUC-JP"); p e.scrub.bytes, e.scrub { |x| "<#{x.bytes}>" }.bytes.size
g = "\x81\x30\x81\x30\xFF\x81".b.force_encoding("GB18030"); p g.valid_encoding?, g.scrub("*").bytes
begin; p t.encode(invalid: :replace, replace: "?").bytes; rescue => ex; p ex.class; end
p "a\xFFb".encode(invalid: :replace).bytes, "a\xFFb".encode(invalid: :replace, replace: "*"), "a\xFFb".encode("UTF-8", invalid: :replace).bytes, "\x80".b.force_encoding("Emacs-Mule").encode("Emacs-Mule", invalid: :replace).bytes
