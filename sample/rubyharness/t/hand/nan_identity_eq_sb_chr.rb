n = Float::NAN; p({a: n} == {a: n}, [n] == [n], n == n, [n].include?(n), {a: n}.eql?({a: n}), [n].eql?([n]), {a: 0.0/0.0} == {a: 0.0/0.0}, [[n]] == [[n]], {a: [n]} == {a: [n]})
p 0x81.chr(Encoding::Windows_1252), 0xF0.chr(Encoding::MacRoman), 0x81.chr(Encoding::Windows_1252).valid_encoding?, (0xF0.chr("macRoman").encode("UTF-8") rescue $!.message), 0x80.chr("Windows-1252").encode("UTF-8")
p((0x81.chr("Windows-1252").encode("UTF-8") rescue $!.message), (0x100.chr("Windows-1252") rescue $!), 0xF0.chr("macRoman").encode("UTF-8", undef: :replace, replace: "").codepoints)
p "a\x81b".force_encoding("Windows-1252").encode("UTF-8", undef: :replace), "a\x81b".force_encoding("Windows-1252").valid_encoding?, "a\x81b".force_encoding("Windows-1252").scrub
p(("\xA5".force_encoding("ISO-8859-3").encode("UTF-8") rescue $!.message), (Encoding::Converter.new("Windows-1252", "UTF-8").convert("\x81") rescue $!.message))
