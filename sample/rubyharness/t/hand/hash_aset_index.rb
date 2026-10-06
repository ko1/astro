# frozen_string_literal: true
# hash[key] = val without the send (plain Hash), frozen Hash, mutable String keys (copied + frozen), a Hash
# subclass and a prepended / redefined []= still dispatching; big indexed Hashes of similar String keys
class SubH < Hash; def []=(k, v); super(k, v * 10); end; end
2.times do |r|
  GC.start if r == 1
  h = {}
  200.times { |i| h["key#{i}"] = i }
  p h.size, h["key0"], h["key57"], h["key199"], h["key200"], h.key?("key100"), h.fetch("key150")
  s = +"mut"; h[s] = :m; s << "x"; p h["mut"], h["mutx"], h.keys.last.frozen?
  f = { a: 1 }.freeze; p((begin; f[:b] = 2; rescue => e; [e.class, e.message]; end))
  sh = SubH.new; sh[:x] = 3; p sh[:x]
  big = {}; 300.times { |i| big["ab#{i}cd"] = i; big[i.to_s] = -i }; p big["ab123cd"], big["299"], big.size
  big.delete("ab5cd"); p big["ab5cd"], big["ab6cd"]; big["ab5cd"] = 55; p big["ab5cd"]
end
class Hash; def []=(k, v); store(k, [:redef, v]); end; end
g = {}; g[:q] = 1; p g
