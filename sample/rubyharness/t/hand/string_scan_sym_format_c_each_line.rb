# Symbol patterns are TypeErrors; %c encodes in the format's encoding;
# each_line/lines with a block iterate over a snapshot of self.
def t(l); r = yield; puts "#{l}: #{r.inspect}"; rescue Exception => e; puts "#{l}: #{e.class}: #{e.message}"; end

t("scan sym") { "cruel world".scan(:test) }
t("scan int") { "cruel world".scan(5) }
t("scan str") { "cruel world".scan("l") }
t("sub sym") { "abc".sub(:b, "x") }
t("gsub sym") { "abc".gsub(:b, "x") }
t("split sym") { "abc".split(:b) }

t("%c eucjp") { ("%c".encode("EUC-JP") % 0x8FABB1).bytes }
t("%c eucjp enc") { ("%c".encode("EUC-JP") % 0xA4A2).encoding }
t("%c ascii big") { "%c".encode("US-ASCII") % 0x8FABB1 }
t("%c ascii ok") { "%c".encode("US-ASCII") % 65 }
t("%c bin") { ("%c".b % 200).bytes }
t("%c bin big") { "%c".b % 256 }
t("%c utf8") { "%c" % 0x3042 }
t("%c utf8 big") { "%c" % 0x110000 }
t("%c neg") { "%c" % -1 }
t("%c str") { "%c" % "あい" }
t("%c width") { "%3c|%-3c" % [65, 66] }

t("each_line snap") { str = +"hello\nworld."; out = []; r = str.each_line { |x| out << x; str[-1] = "!" }; [r, out] }
t("lines snap") { str = +"hello\nworld."; out = []; r = str.lines { |x| out << x; str[-1] = "!" }; [r, out] }
t("each_line replace") { str = +"a\nb\nc"; out = []; str.each_line { |x| out << x; str.replace("zz") }; out }
t("each_line chomp") { out = []; "a\r\nb\n".each_line(chomp: true) { |x| out << x }; out }
