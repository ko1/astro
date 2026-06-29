p "hello".delete_prefix("hel")
p "hello".delete_suffix("llo")
class TS; def to_str; "he"; end; end
p "hello".delete_prefix(TS.new)
class TE; def to_str; "lo"; end; end
p "hello".delete_suffix(TE.new)
def t; yield; rescue TypeError; "TE"; end
p t { "x".delete_prefix(5) }
p "hello".delete_prefix("xyz")
