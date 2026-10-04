# String#tr builds its result directly (no stdio stream): ranges, negation, deletion, multibyte growth, binary
p "hello world".tr("a-y", "b-z"), "hello".tr("el", "ip"), "hello".tr("^l", "*"), "hello".tr("lo", "")
p "aaa".tr("a", "é"), "éaé".tr("é", "e"), "日本語".tr("本", "x"), "abc".tr("a-c", "あ-う")
p "\xff\x01".b.tr("\x01", "\x02"), "\xff\x01".b.tr("\x01", "\x02").encoding
big = "abc" * 400
p big.tr("abc", "xyé").bytesize, big.tr("abc", "xyé")[0, 9]
p "".tr("a", "b"), "x".tr("", "y"), "^".tr("^", "v")
s = "frozen".freeze
p s.tr("o", "0"), s
3.times { |i| GC.start if i == 1; p ("ab" * 300).tr("a", "ü").bytesize }
