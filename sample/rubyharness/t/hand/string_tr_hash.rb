# Ruby 4.1 String#tr(hash) / #tr!(hash).
# The empty-hash / no-match cases are deliberately NOT here: CRuby 4.1.0dev
# (2026-09-11) gets them wrong (`"x".tr({})` answers nil, `tr!` empties the
# receiver) and this corpus diffs against CRuby.  core/string/tr_spec.rb covers
# them and koruby passes 40/40 there, where CRuby itself is 36/40.
def t(tag); r = yield; puts "#{tag}: => #{r.inspect}"; rescue => e; puts "#{tag}: #{e.class}: #{e.message}"; end
t("basic")      { "hello".tr("e" => "EH! ", "o" => "OH!", "l" => "") }
t("utf8")       { "h€llø".tr("€" => "e", "ø" => "o") }
t("tr! hit")    { s = +"hello"; [s.tr!("l" => "L"), s] }
t("enc US")     { s = "hello".encode(Encoding::US_ASCII).tr("e" => "é"); [s, s.encoding.to_s] }
t("multi key")  { "hello".tr("hel" => "") }
t("nonstr key") { "hello".tr(1 => "x") }
t("nonstr val") { "hello".tr("h" => 1) }
t("1 arg str")  { "hello".tr("a") }
t("3 args")     { "hello".tr("a","b","c") }
t("hash+2nd")   { "hello".tr({"a"=>"b"}, "c") }
t("frozen tr!") { s = "x".freeze; s.tr!("x" => "y") }
t("incompat k") { "fée".tr("é".encode(Encoding::ISO_8859_1) => "e") }
t("incompat v") { "fée".tr("e" => "é".encode(Encoding::ISO_8859_1)) }
t("two-set")    { "hello".tr("el", "ip") }
t("two-set neg"){ "hello".tr("^aeiou", "*") }
t("utf16")      { "hello".encode(Encoding::UTF_16LE).tr("e".encode(Encoding::UTF_16LE) => "EH! ".encode(Encoding::UTF_16LE), "o".encode(Encoding::UTF_16LE) => "OH!".encode(Encoding::UTF_16LE), "l".encode(Encoding::UTF_16LE) => "".encode(Encoding::UTF_16LE)) == "hEH! OH!".encode(Encoding::UTF_16LE) }
