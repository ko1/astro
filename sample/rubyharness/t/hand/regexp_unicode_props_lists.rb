# \p{Age=3.2}, long general-category names (\p{private use}), blocks (\p{in Tags}), all scripts; a long
# \u{...} list in and out of a class (net-imap stringprep tables)
tests = { "" => '\p{private use}', "a" => '\p{letter}', "퟿" => '\p{surrogate}', "￾" => '\p{noncharacter code point}',
          "￰" => '\p{in specials}', "⿰" => '\p{in ideographic description characters}', "\u{E0041}" => '\p{in Tags}',
          "\u{1F600}" => '\p{Extended Pictographic}', "\u{1F3FB}" => '\p{Emoji Modifier}', "x" => '\p{^NChar}', "א" => '\p{Hebrew}',
          "\u{16A0}" => '\p{Runic}', "\u{10330}" => '\p{Gothic}', "\u{1E900}" => '\p{Adlam}', "Ā" => '\p{In_Latin_Extended_A}',
          "A" => '\p{Uppercase_Letter}', "_" => '\p{Connector_Punctuation}', "͸" => '\p{Unassigned}' }
tests.each { |s, re| r = begin; (s =~ Regexp.new(re)).inspect; rescue RegexpError => e; e.message; end; puts "#{re}: #{r}" }
cps = (0x100..0x17f).to_a
src = "[\\u{" + cps.map { |c| c.to_s(16) }.join(" ") + "}]"
re = Regexp.new(src)
p "Ő" =~ re, "a" =~ re, ("Ő".."Œ").count { |ch| ch =~ re }
lit = Regexp.new("\\u{" + (0x61..0x61+300).map { |c| c.to_s(16) }.join(" ") + "}")
s = (0x61..0x61+300).map { |c| c.chr(Encoding::UTF_8) }.join
p s =~ lit, (s + "x") =~ lit

p("é" =~ /\p{age=3.2}/, "é" =~ /\p{^AGE=3.2}/, "\u{1F600}" =~ /\P{age=6.1}/)
