# encoding: utf-8
# interpolation of several parts starts in the first part's encoding (CRuby's rb_str_concat_literals); one part keeps the source's
b = 12; u = "é"; ascii = "ab".force_encoding("US-ASCII"); bin = "\xff".b; sj = "x".encode("Shift_JIS")
p "#{ascii}#{b}".encoding, "#{ascii}".encoding, "#{b}".encoding, "#{b}#{ascii}".encoding, "#{:s}".encoding
p "#{ascii}#{u}".encoding, "x#{ascii}".encoding, "#{bin}".encoding, "#{bin}#{b}".encoding, "#{sj}#{b}".encoding
p "#{nil}#{ascii}".encoding, "#{ascii}#{nil}".encoding
p (/#{ascii}/).encoding, (/#{ascii}#{u}/).encoding, (/#{sj}/).encoding
