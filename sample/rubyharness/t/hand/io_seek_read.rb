# IO#read(n) / seek / pos / pos= / rewind / tell / each_char. vs ruby.
D = "/tmp/kioc"
File.open("#{D}/s.txt", "w") { |f| f.write("0123456789abcdef") }
File.open("#{D}/s.txt", "r") do |f|
  p f.read(4)
  p f.pos
  f.seek(10)
  p f.read(3)
  f.pos = 2
  p f.read(2)
  f.rewind
  p f.tell
  p f.read(5)
end
File.open("#{D}/s.txt", "r+") { |f| f.seek(0); f.write("XYZ") }
p File.read("#{D}/s.txt")[0, 5]
p File.open("#{D}/s.txt") { |f| f.each_char.first(4) }
seen = []
File.open("#{D}/s.txt") { |f| f.each_char { |ch| seen << ch } }
p seen.length
p File.open("#{D}/s.txt") { |f| f.read(3); f.read }
File.delete("#{D}/s.txt")
