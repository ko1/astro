# builtin の subclass が演算子を上書きしたら、ノードの fast path ではなく
# その定義が呼ばれる (rack の Rack::Headers < Hash が self.[] と []= でキーを
# 正規化しており、これが効かないとヘッダ名が小文字にならない)。
class DownHash < Hash
  def self.[](h); x = new; h.each { |k, v| x[k] = v }; x; end
  def [](k); super(k.is_a?(String) ? k.downcase : k); end
  def []=(k, v); super(k.is_a?(String) ? k.downcase : k, v); end
  def delete(k); super(k.is_a?(String) ? k.downcase : k); end
end
h = DownHash["AB" => 1, "Cd" => 2]
p h.keys
p h["ab"], h["AB"]
h["EF"] = 3
p h.keys
p h.delete("eF")
p h.class

class UpAry < Array
  def self.[](*a); x = new; a.each { |e| x << e }; x; end
  def <<(x); super(x.to_s.upcase); end
  def [](i); super(i); end
  def []=(i, v); super(i, v.to_s.upcase); end
  def +(o); super(o.map { |e| e.to_s.upcase }); end
end
a = UpAry["q", "r"]
p a
a << "s"
a[0] = "t"
p a
p a + ["z"]
p a[1]
p a.class

# 素の builtin は今までどおり fast path
plain = [1, 2, 3]
plain << 4
plain[0] = 9
p plain, plain[1], plain + [5]
ph = { "K" => 1 }
ph["L"] = 2
p ph, ph["K"]
