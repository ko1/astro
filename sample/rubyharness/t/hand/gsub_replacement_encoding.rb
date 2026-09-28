# sub/gsub with a String replacement: its encoding joins the result (a binary replacement
# with high bytes makes an ASCII-only UTF-8 subject binary; two non-ASCII sides clash)
data = (0..255).map(&:chr).join * 2
p data.encoding
e = "a\n<data>\nb".gsub(/\n/, "\r\n")
e.sub!(/<data>/, data)
p e.encoding, e.valid_encoding?
e2 = e.gsub(/<boundary>/, "zz"); p e2.encoding
p "x<d>".sub(/<d>/, data).encoding, "x<d>".sub("<d>", data).encoding, "x<d>".gsub(/<d>/, data).encoding
p "é<d>".sub(/<d>/, "a".b).encoding
begin; p "é<d>".sub(/<d>/, "\xff".b).encoding; rescue => x; p x.class; end
