p "hello".dump.undump
p "a\nb\tc".dump.undump
p "日本語".dump.undump
p "with \"quote\"".dump.undump
p "back\\slash".dump.undump
p "tab\there".dump.undump
p "\e[0m".dump.undump
def t; yield; rescue RuntimeError; "RE"; end
p t { "no quotes".undump }
p "\"abc\"".undump
p "abc".dump.undump.frozen?
