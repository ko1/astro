# A batch of String / Symbol / Regexp CRuby-compatibility details.
def t(label); r = yield; puts "#{label}: #{r.inspect}"; rescue Exception => e; puts "#{label}: #{e.class}: #{e.message}"; end

# to_c: a single '_' joins digits; "__" or a trailing '_' ends the number
t("to_c 1") { '79+4iruby'.to_c }
t("to_c 2") { '7__9+4__0i'.to_c }
t("to_c 3") { "5+3_1i".to_c }
t("to_c 4") { "5+3__1i".to_c }
t("to_c 5") { "12_3".to_c }
t("to_c 6") { "12__3".to_c }
t("to_c 7") { "1_2.3_4e1_0i".to_c }

# tr / tr_s: result keeps self's encoding; incompatible sets raise
t("tr enc") { "hello".encode("US-ASCII").tr("l", "r").encoding }
t("tr_s enc") { "hello".encode("US-ASCII").tr_s("l", "r").encoding }
t("tr compat") { "hello".tr("l".encode("UTF-16LE"), "r") }
t("tr_s compat") { "hello".tr_s("l", "r".encode("UTF-16LE")) }
t("tr_s ok") { "hello".tr_s("l", "r") }

# Regexp whose source is non-ASCII is fixed to its encoding
re = Regexp.new("れ".encode(Encoding::EUC_JP))
t("re enc") { re.encoding }
t("=~ eucjp") { "あれ" =~ re }
t("index eucjp") { "あれ".index(re) }
t("rindex eucjp") { "あれ".rindex(re) }
t("ascii subj") { "abc" =~ re }
t("utf16 subj") { "hello".encode("UTF-16LE") =~ /e/ }

# str[re, -n]: a negative capture index can't reach group 0
t("[re,-1]") { "hello there"[/hello (.)/, -1] }
t("[re,-2]") { "hello there"[/hello (.)/, -2] }
t("[re,2]") { "hello there"[/hello (.)/, 2] }

# str[i, bignum]
t("[4,-2**63]") { "hello there"[4, -(1 << 63)] }
t("[4,-2**31]") { "hello there"[4, -(1 << 31)] }
t("[4,2**63]") { "hello there"[4, 1 << 63] }
t("[4,2**64]") { "hello there"[4, 1 << 64] }
t("[4,2**62]") { "hello there"[4, 1 << 62] }

# scrub!: a valid (even frozen) string is left alone; an invalid frozen one raises
t("scrub! frozen valid") { s = "a".freeze; r = s.scrub!; [r, s.frozen?, r.equal?(s)] }
t("scrub! frozen invalid") { s = "a\x81".force_encoding("utf-8").freeze; s.scrub! }
t("scrub! ivar") { s = "a"; s.instance_variable_set(:@a, 'b'); s.scrub!; s.instance_variable_get(:@a) }
t("scrub! invalid") { s = "a\x81".force_encoding("utf-8"); s.scrub!; s }

# split("") with a limit larger than the char count keeps a trailing ""
t("split '' 4") { "hi!".split("", 4) }
t("split '' -1") { "hi!".split("", -1) }
t("split '' 3") { "hi!".split("", 3) }
t("split '' 2") { "hi!".split("", 2) }

# partition / rpartition: empty parts keep self's encoding
t("partition enc") { "hello".dup.force_encoding(Encoding::US_ASCII).partition("é").map(&:encoding) }
t("rpartition enc") { "hello".dup.force_encoding(Encoding::US_ASCII).rpartition("é").map(&:encoding) }

# ord on a broken first character
t("ord usascii") { "\xC2".force_encoding("US-ASCII").ord }
t("ord utf8") { "\xC2".force_encoding("UTF-8").ord }
t("ord binary") { "\xC2".b.ord }

# Symbol#casecmp?: Unicode folding for UTF-8, ASCII-only otherwise, nil across encodings
t("sym Ä ä") { :"Ä".casecmp?(:"ä") }
t("sym ß SS") { :"ß".casecmp?(:"SS") }
t("sym latin1") { "\xC3".b.to_sym.casecmp?("\xE3".b.to_sym) }
t("sym mixed") { "\xC3".b.to_sym.casecmp?(:"ã") }
t("sym non-sym") { :a.casecmp?("a") }
t("str latin1") { "\xC3".b.casecmp?("\xE3".b) }

# String#match delegates to a Regexp subclass's #match
k = Class.new(Regexp) { def match(*a); $rec = [:match, *a]; super; end }
t("match sub") { r = "hello".match(k.new("l+")); [r[0], $rec] }
t("match sub pos") { r = "hello".match(k.new("l"), 3); [r.begin(0), $rec] }
