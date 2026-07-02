# IO.read/write/readlines/foreach + File.binread/binwrite + read(len,offset) +
# File.foreach Enumerator + IO#getc/readline/readchar + readlines(chomp:). vs ruby.
D = "/tmp/koruby_iocm"
Dir.mkdir(D) unless Dir.exist?(D)
File.write("#{D}/x.txt", "hello\nworld\n")
p IO.read("#{D}/x.txt")
p IO.readlines("#{D}/x.txt")
p File.binread("#{D}/x.txt")
p File.binwrite("#{D}/y.txt", "bin")
p File.read("#{D}/y.txt")
p IO.write("#{D}/z.txt", "iowrite")
p File.read("#{D}/z.txt")
p File.read("#{D}/x.txt", 5)
p File.read("#{D}/x.txt", 5, 6)
p File.foreach("#{D}/x.txt").to_a
p File.readlines("#{D}/x.txt", chomp: true)
File.open("#{D}/x.txt") { |f| p f.readline; p f.getc; p f.readchar }
File.delete("#{D}/x.txt", "#{D}/y.txt", "#{D}/z.txt")
Dir.rmdir(D)
