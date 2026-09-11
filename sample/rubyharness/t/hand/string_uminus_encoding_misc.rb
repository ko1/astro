# String#-@ interning, Encoding.compatible? with a Regexp, Encoding.find name
# checks, Encoding.locale_charmap, Symbol#start_with?/end_with? encodings.
def t(label); r = yield; puts "#{label}: #{r.inspect}"; rescue Exception => e; puts "#{label}: #{e.class}: #{e.message}"; end

origin = "this is a string"; dynamic = %w(this is a string).join(" ")
t("uminus dyn") { [origin.equal?(dynamic), (-origin).equal?(-dynamic), (-origin).frozen?, (-origin) == origin] }
t("uminus lit") { [(-"unfrozen string").equal?(-"unfrozen string"), (-"unfrozen string").equal?(-"another unfrozen string")] }
t("uminus frozen self") { i = "foo".freeze; (-i).equal?(i) }
t("uminus unfrozen copy") { i = "foo"; o = -i; [o.frozen?, o.equal?(i), o == "foo"] }
t("uminus ivars") { d = %w(this string is frozen).join(" "); d.instance_variable_set(:@a, 1); d.freeze
                     [(-d).equal?((-"this string is frozen").freeze), (-d).equal?(-d), (-d).equal?(d)] }
t("uminus subclass") { k = Class.new(String); s = k.new("sub"); o = -s; [o.class == String, o.frozen?, o == "sub"] }
t("dedup alias") { "dedup me".dedup.equal?(-"dedup me") }

r = Regexp.new("\xa4\xa2".dup.force_encoding("euc-jp"))
t("compat re,str") { Encoding.compatible?(r, "hello".dup.force_encoding("utf-8")) }
t("compat str,re") { Encoding.compatible?("hello".dup.force_encoding("utf-8"), r) }
t("compat ascii re") { Encoding.compatible?(/abc/, "x".dup.force_encoding("euc-jp")) }
t("compat non-ascii str") { Encoding.compatible?("あ", r) }
t("compat same") { Encoding.compatible?(r, "\xa4\xa2".dup.force_encoding("euc-jp")) }
t("compat enc,enc") { Encoding.compatible?(Encoding::US_ASCII, Encoding::UTF_8) }
t("compat empty") { Encoding.compatible?("", "\xff".b) }

t("find utf16 name") { Encoding.find("utf-8".encode("utf-16be")) }
t("find non-ascii") { Encoding.find("utf-8é") }
t("find ok") { Encoding.find("utf-8") }

t("charmap") { Encoding.locale_charmap }
t("charmap after ENV") { old = ENV["LC_ALL"]; ENV["LC_ALL"] = "C"; r = Encoding.locale_charmap; ENV["LC_ALL"] = old; r }

t("sym end_with utf16") { "\xd8\x00\xdc\x00".dup.force_encoding("UTF-16BE").to_sym.end_with?("\xdc\x00".dup.force_encoding("UTF-16BE")) }
t("sym end_with mb") { "\xe3\x81\x82".to_sym.end_with?("\x82") }
t("sym end_with ok") { :"あい".end_with?("い") }
t("sym start_with utf16") { "\xd8\x00\xdc\x00".dup.force_encoding("UTF-16BE").to_sym.start_with?("\xd8\x00".dup.force_encoding("UTF-16BE")) }
t("sym start_with ok") { :hello.start_with?("he", "x") }
