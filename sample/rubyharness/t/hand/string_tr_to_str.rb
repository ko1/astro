p "hello".tr("el", "ip")
class TS; def to_str; "l"; end; end
p "hello".tr(TS.new, "L")
class TS2; def to_str; "z"; end; end
p "hello".tr("l", TS2.new)
p "aabbcc".tr_s("a-c", "x")
def t; yield; rescue TypeError; "TE"; end
p t { "x".tr(5, "y") }
