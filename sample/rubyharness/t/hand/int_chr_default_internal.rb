# Integer#chr with no argument: Encoding.default_internal decides only for codepoints > 255
def show(n) = (s = n.chr; [s.bytes, s.encoding.to_s]) rescue [$!.class, $!.message]
codes = [65, 0x7f, 0x80, 0xff, 0x100, 0x3042]
p codes.map { show(_1) }
Encoding.default_internal = Encoding::UTF_8
p codes.map { show(_1) }
Encoding.default_internal = Encoding::EUC_JP
p codes.map { show(_1) }
Encoding.default_internal = nil
p codes.map { show(_1) }
p 0x3042.chr(Encoding::UTF_8).encoding.to_s, 65.chr("US-ASCII").encoding.to_s
