# JSON.parse / JSON.generate through koruby's native path (builtins/json_native.c) and the
# Ruby fallbacks it hands over to must look the same as CRuby's json.
require "json"

doc = %q({"a":[1,-0,2.5,1e400,-1.5e-3,12345678901234567890,0.1,"xé\n\"",true,null,{}],"b":{"c":[[]]},"d":"😀"})
v = JSON.parse(doc)
p v, v["a"].map(&:class)
p JSON.parse(doc, symbolize_names: true)[:b]
f = JSON.parse(doc, freeze: true)
p f.frozen?, f["a"].frozen?, f["a"][7].frozen?, f.keys.all?(&:frozen?)
p JSON.parse(" [1 , 2 ] "), JSON.parse('"s"'), JSON.parse("3"), JSON.parse("null")
p JSON.parse("[NaN, Infinity, -Infinity]", allow_nan: true).map(&:to_s)
p JSON.parse("/* c */ [1, // d\n 2,]", allow_comments: true, allow_trailing_comma: true)
p JSON.parse(%q({"a":1,"a":2}), allow_duplicate_key: true)
[%q({"a":1,}), %q({"a":1,"a":2}), "[1,]", "[01]", %q(["\u0001"]), "", "[", %q({"a" 1})].each do |bad|
  begin; JSON.parse(bad); rescue JSON::ParserError => e; p e.message; end
end
p JSON.parse(%q({"o":{"x":1}}), object_class: Hash, on_load: ->(x) { x })
keys = JSON.parse(Array.new(40) { |i| %Q("k#{i % 7}":#{i}) }.join(",").then { "{#{_1}}" }, allow_duplicate_key: true)
p keys.size, keys["k3"]
big = { "arr" => Array.new(30) { |i| { "id" => i, "name" => "n#{i}", "f" => i / 7.0 } } }
p JSON.parse(JSON.generate(big)) == big

o = { "s" => "q\"b\\s/\u0001\t", "n" => [1, -2, 3.0, 1e20, 1.0e-7, 2**70], "u" => "日本😀", "e" => [], "h" => {} }
puts JSON.generate(o), JSON.pretty_generate(o), o.to_json
puts JSON.generate(o, indent: "\t", space: " ", object_nl: "\n", array_nl: "\n", space_before: " ")
puts JSON.generate({ a: 1, b: { c: :d } })
puts JSON.generate([Float::NAN], allow_nan: true)
p((JSON.generate([Float::INFINITY]) rescue $!.class), (JSON.generate(["\xff".b]) rescue $!.class))
p((JSON.generate([[[[1]]]], max_nesting: 2) rescue $!.class))
class JsonPathsPt; def to_json(*) = %q("pt"); end
puts JSON.generate([JsonPathsPt.new, { "k" => JsonPathsPt.new }])

# $~ set by byteindex(Regexp) stays in the method that called it
def json_paths_byteindex(s, re) = s.byteindex(re, 0)
if "pretty:x" =~ /\A(\w+):/
  json_paths_byteindex("abc", /b/)
  JSON.parse('{"a":"b"}')
  p $1
end
