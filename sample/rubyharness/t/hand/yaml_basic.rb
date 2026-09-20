# YAML (psych) — koruby は libyaml への橋渡し (builtins/yaml_bridge.c) だけを C で持ち、
# psych の Ruby 実装 (lib/psych/) はそのまま載せている。
require "yaml"

p YAML.dump({"a" => 1, "b" => [1, 2, {"c" => nil}], "d" => "x y", :sym => :v})
p YAML.dump([1, "two", 3.5, true, nil, :s, "", "123", "yes"])
p YAML.load("a: 1\nb:\n - 1\n - 2\nc: ~\nd: 'q'\ne: \"r\"\n")
p YAML.load("--- []\n")
p YAML.load("--- {}\n")
p YAML.safe_load("---\n- 1\n- two\n- 3.5\n- true\n")
begin
  YAML.safe_load("--- !ruby/object:Object {}\n")
rescue => e
  p e.class.to_s
end
p YAML.unsafe_load("--- !ruby/symbol foo\n")
# anchor / alias
p YAML.load("a: &x 1\nb: *x\n", aliases: true)
# block scalars
p YAML.load("a: |\n  l1\n  l2\nb: >\n  f1\n  f2\n")
# 複数ドキュメント
p YAML.load_stream("--- 1\n--- 2\n")
# 往復
[["x", 1, :a, nil, true, 1.5], {"k" => "v"}, [[1, [2, [3]]]], {1 => {2 => 3}}].each do |o|
  p YAML.load(YAML.dump(o)) == o
end
# 例外の往復 (message だけ)
e = YAML.unsafe_load(YAML.dump(RuntimeError.new("boom")))
p [e.class.to_s, e.message]
# Struct
S = Struct.new(:a, :b)
p YAML.unsafe_load(YAML.dump(S.new(1, 2))).to_a
# エラー
begin
  YAML.load("a:\n-\n - b\n\tc: 1\n")
rescue Psych::SyntaxError => e
  p [e.class.to_s, e.line.is_a?(Integer), e.column.is_a?(Integer)]
end
p YAML.dump("日本語")
p YAML.load(YAML.dump("日本語"))
p Psych::VERSION
