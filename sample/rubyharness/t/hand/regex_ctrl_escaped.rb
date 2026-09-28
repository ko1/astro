# \c / \C- take an escaped value too (\c\xFF, \c\101): Onigmo's X & 0x9F
[ "\\c\\xFF", "\\c\\xFF".b, "\\cA", "\\C-a", "\\c\\101", "[\\cA-\\cZ]" ].each do |s|
  r = begin; re = Regexp.new(s); [re.source, re.match?("\x01".b.force_encoding(s.encoding)) ]; rescue RegexpError => e; e.message; end
  p r
end
p "\x01" =~ /\cA/, "\x1f" =~ /\c?/
