# frozen_string_literal: true
# Array#dup / Hash#dup copy the elements in one go: small and big, compare_by_identity, Float / heap keys
# (no index), an Array key mutated after insertion (key hints), defaults, a subclass and independence of the copy
2.times do |r|
  GC.start if r == 1
  [0, 1, 6, 7, 100].each do |n|
    a = (0...n).map { |i| "e#{i}" }; d = a.dup
    d << :x; p [n, d.size, a.size, d[0...n] == a, d.frozen?, d.equal?(a)]
  end
  [0, 3, 15, 16, 17, 40].each do |n|
    h = {}; n.times { |i| h[:"k#{i}"] = i }; d = h.dup
    d[:new] = 1; p [n, d.size, h.size, d[:"k#{n - 1}"], d.key?(:new), h.key?(:new), d.keys.first]
  end
  big = {}; 30.times { |i| big["s#{i}"] = i }; bd = big.dup; p [bd["s0"], bd["s29"], bd["s30"]]
  fl = { 1.0 => :f, 2 => :i }; 20.times { |i| fl[i + 100] = i }; fd = fl.dup; p [fd[1], fd[1.0], fd[2], fd[119]]
  id = {}.compare_by_identity; k1 = +"a"; k2 = +"a"; id[k1] = 1; id[k2] = 2
  idd = id.dup; p [idd.compare_by_identity?, idd.size, idd[k1], idd[k2], idd["a"]]
  bid = {}.compare_by_identity; 20.times { |i| bid[i.to_s] = i }; p bid.dup.size
  ak = [1]; hk = { ak => :v }; ak << 2; hd = hk.dup; p [hk[[1, 2]], hd[[1, 2]], hk.rehash[[1, 2]]]
  bk = {}; keys = (0...20).map { |i| [i] }; keys.each { |k| bk[k] = k[0] }; keys[3] << 9; bkd = bk.dup
  p [bk[[3, 9]], bkd[[3, 9]], bkd[[5]]]; 30.times { |i| bkd[[i, i]] = i }; p [bkd[[5]], bkd[[29, 29]], bkd.size, bkd.rehash[[3, 9]]]
  dv = Hash.new(5); dv[:a] = 1; p [dv.dup[:zz], Hash.new { |_, k| k.to_s * 2 }.dup[:ab]]
  class SubA < Array; end; class SubHa < Hash; end
  sa = SubA.new; sa << 1; sh = SubHa.new; sh[:a] = 1; p [sa.dup.class, sa.dup, sh.dup.class, sh.dup]
  fz = { a: 1 }.freeze; p [fz.dup.frozen?, [1].freeze.dup.frozen?]
end
