S3 = Struct.new(:a, :b, :c)
s = S3.new(1, 2, 3)
p s.each_pair.to_a
r = []; s.each_pair { |k, v| r << "#{k}=#{v}" }; p r
p s.each_pair.map { |k, v| "#{k}:#{v}" }
P2 = Struct.new(:x, :y)
p P2.new(3, 4).each_pair.to_h
p S3.new(1,2,3).each_pair.select { |k, v| v.odd? }
p S3.new(10,20,30).each_pair.reduce(0) { |sum, (k, v)| sum + v }
KV = Struct.new(:key, :value, keyword_init: true)
p KV.new(key: "a", value: 1).each_pair.to_a
