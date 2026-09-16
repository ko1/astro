# The default Object#inspect never consults #to_s — not even for an object with
# no instance variables, where the generic formatter would otherwise fall
# through to the #to_s path.
def t(tag); r = yield; puts "#{tag}: => #{r.inspect}"; rescue => e; puts "#{tag}: #{e.class}: #{e.message}"; end
class T; def to_s = "TS"; end
class I; def inspect = "INS"; end
class TI; def to_s = "TS"; def inspect = "INS"; end
class V; def initialize; @a = 1; @b = "x"; end; def to_s = "TS"; end
class N; end
n = ->(s) { s.gsub(/0x\h+/, "0xA") }
t("to_s only")    { n.(T.new.inspect) }
t("inspect only") { I.new.inspect }
t("both")         { TI.new.inspect }
t("ivars + to_s") { n.(V.new.inspect) }
t("neither")      { n.(N.new.inspect) }
t("in array")     { n.([T.new].inspect) }
t("in hash")      { n.(({k: T.new}).inspect) }
t("in struct")    { S = Struct.new(:x); n.(S.new(T.new).inspect) }
t("to_s stands")  { "#{T.new}" }
t("puts uses to_s") { require 'stringio'; io = StringIO.new(+""); io.puts(T.new); io.string }
