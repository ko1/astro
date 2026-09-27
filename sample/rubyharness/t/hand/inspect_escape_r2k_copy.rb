o = Object.new; def o.inspect = %<"utf_16be あ">.encode(Encoding::UTF_16BE)
p({a: o}.inspect, [o].inspect)
p [o]
e = Encoding.default_external
Encoding.default_external = Encoding::ASCII
p eval('{"\\u3042": 1}').inspect, [:"あ"].inspect, :"あ".inspect, "あ".inspect, ["あ"].inspect
Encoding.default_external = e
p({"あ": 1}.inspect)
o = Object.new; def o.inspect = "a\tb\x00\x7fあ\u{1F600}".encode("UTF-16LE")
p [o]
b = Object.new; def b.inspect = "x\xff\xfey".b
p [b]
e = Object.new; def e.inspect = "あ".encode("EUC-JP")
p [e], {k: e}
class << self; ruby2_keywords def mark(*a) = a; end
def single(arg) = arg
def rest(*args) = args
h = { a: 1 }
args = mark(**h)
marked = args.last
p Hash.ruby2_keywords_hash?(marked)
ar = rest(*args)
p ar.last.equal?(marked), Hash.ruby2_keywords_hash?(ar.last)
def kw(**kw) = kw
p kw(*args).equal?(marked)
p [1, 2].inspect.encoding, [].inspect.encoding, ["a"].inspect.encoding, [:a].inspect.encoding, {a: 1}.inspect.encoding, ["jp".encode("EUC-JP"), "u"].inspect.encoding, [nil].inspect.encoding, [1.5].inspect.encoding
p 1.inspect.encoding, nil.inspect.encoding, "a".inspect.encoding, :a.inspect.encoding, 1.5.inspect.encoding, (1..2).inspect.encoding
