# count/delete/squeeze reject encoding-incompatible sets; << Integer in a
# multibyte non-UTF-8 encoding; to_f with "1.e-2".
def t(l); r = yield; puts "#{l}: #{r.inspect}"; rescue Exception => e; puts "#{l}: #{e.class}: #{e.message}"; end

u16 = "l".encode("UTF-16LE")
t("count compat") { "hello".count(u16) }
t("delete compat") { "hello".delete(u16) }
t("squeeze compat") { "hello".squeeze(u16) }
t("count ok") { "hello".count("l", "lo") }
t("delete ok") { "hello".delete("l", "lo") }
t("squeeze ok") { "yellow moon".squeeze("lo") }
t("count sym") { "hello".count(:l) }
t("delete empty self") { "".delete(u16) }

t("<< eucjp 0x81") { "".encode(Encoding::EUC_JP) << 0x81 }
t("<< eucjp ok") { ("".encode(Encoding::EUC_JP) << 0xA4A2).bytes }
t("<< eucjp ascii") { ("".encode(Encoding::EUC_JP) << 65) }
t("<< usascii 256") { "".encode(Encoding::US_ASCII) << 256 }
t("<< usascii 200") { ("".encode(Encoding::US_ASCII) << 200).encoding }
t("<< utf8 big") { "" << 0x110000 }

t("to_f 1.") { "1.".to_f }
t("to_f 1.e+0") { "1.e+0".to_f }
t("to_f 1.e-2") { "1.e-2".to_f }
t("to_f 1.e5") { "1.e5".to_f }
t("to_f 1.ex") { "1.ex".to_f }
t("to_f .e5") { ".e5".to_f }
t("to_f 1_0.e1") { "1_0.e1".to_f }
