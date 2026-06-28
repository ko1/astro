S1 = Struct.new(:a, keyword_init: true)
S2 = Struct.new(:a, keyword_init: false)
S3 = Struct.new(:a)
p S1.keyword_init?
p S2.keyword_init?
p S3.keyword_init?
p S1.new(a: 5).a
p S3.new(7).a
