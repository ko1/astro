# A start position handed to a Regexp match is a character index IN THE
# SUBJECT'S OWN ENCODING.  Walking a BINARY string as if it were UTF-8 skipped
# past every multi-byte-looking run — marcel's XML sniffer scans a transcoded
# prefix with `data.index(/["'\[\]<>]/n, pos)` exactly that way.
b = "aa\xE2\x80\x90]>".b
p [b.encoding, b.bytesize, b.length]
[0, 3, 5, 6].each do |i|
  p [i, b.index(/\]/n, i), b.index(/\]/, i), b.index("]", i),
     b.match(/\]/n, i)&.begin(0), b.match?(/\]/n, i), /\]/n.match(b, i)&.begin(0),
     /\]/n.match?(b, i), b.rindex(/\]/n, i), b.byteindex(/\]/n, i)]
end

u = "aa‐]>"
p [u.length, u.bytesize]
[0, 2, 3, 4].each do |i|
  p [i, u.index(/\]/, i), u.match(/\]/, i)&.begin(0), u.rindex(/\]/, i)]
end

# the same scan marcel runs
data = %(<!DOCTYPE rss [<!-- ‐ -->]><rss/>).b
dd = /["'\[\]<>]/n
pos = 0
out = []
while (i = data.index(dd, pos))
  out << [i, data.getbyte(i).chr]
  pos = i + 1
end
p out

lat = "aa\xE9]>".dup.force_encoding("ISO-8859-1")
p [lat.length, lat.index(/\]/, 3), lat.index(/\]/, 2)]
