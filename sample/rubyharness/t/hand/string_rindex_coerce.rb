class S; def to_str; "l"; end; end
class TI; def to_int; 1; end; end
checks = []
checks << "hello".rindex("l")
checks << "hello".rindex("l", 2)
checks << (begin; "hello".rindex(123); rescue => e; e.class.to_s; end)
checks << (begin; "hello".rindex([]); rescue => e; e.class.to_s; end)
checks << "hello".rindex(S.new)
checks << (begin; "hello".rindex(TI.new); rescue => e; e.class.to_s; end)
p checks
def t(s, *args); s.rindex(*args); rescue => e; e.class; end
p t("", 1)
p t("", 1, -1)
p t("hello", 1)
p t("hello", 1, -1)
p t("hello", 1, 2)
p t("hi", "x", -99)
p t("hello", "l", -99)
