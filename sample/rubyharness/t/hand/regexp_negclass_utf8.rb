# UTF-8 では [^...] は「1 文字」にマッチする。astrogre はバイト単位で反転していて、
# 多バイト文字の先頭バイトだけを食ってブロックに壊れた String を渡していた
# (builder の String#to_xs が c.ord で ArgumentError)。
s = "aéb\u{1F600}c"
p s.gsub(/[^\x00-\x7F]/) { |c| "<#{c.bytes.inspect}>" }
p s.scan(/[^\x00-\x7F]/)
p s.gsub(/[^a-z]/) { |c| "<#{c.bytes.size}>" }
p(s =~ /[^\x00-\x7F]/)
p s.index(/[^a-z]/)
p s.split(/[^a-z]/)
p "aあb".sub(/[^a]/) { |c| c.ord.to_s }
p "abc".gsub(/[^b]/, "-")
p "é".match?(/\A[^a]\z/)
# binary はバイト単位のまま (scan / gsub の前進もバイト 1 個)
p "AB".gsub(/[^[:lower:]]/) { |c| c.downcase }
b = "a\xC3\xA9b".dup.force_encoding("BINARY")
p b.bytes, b.encoding.to_s
p b.scan(/[^a-z]/).map(&:bytes)
p b.scan(/[^a]/).map(&:bytes)
p b.scan(/./m).map(&:bytes)
p b.gsub(/[^a-z]/) { |c| c.bytes.inspect }
p b.index(/[^a-z]/)
p b.split(/[^a-z]/).map(&:bytes)
u = "aéb"
p u.scan(/[^a-z]/)
