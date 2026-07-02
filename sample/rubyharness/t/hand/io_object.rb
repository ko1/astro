# IO/File objects: File.open (block, auto-close), write/read/gets/each_line,
# $stdout/STDOUT. vs ruby.
T = "/tmp/koruby_io_#{$$}"
File.open(T, "w") { |f| f.puts "line1"; f.puts "line2"; f.print "noeol" }
p File.read(T)
File.open(T, "r") { |f| p f.read }
File.open(T, "r") { |f| p f.gets; p f.gets }
File.open(T) { |f| p f.readlines }
File.open(T) { |f| p f.each_line.to_a }
lines = []
File.open(T) { |f| f.each_line { |l| lines << l.chomp } }
p lines
File.open(T, "a") { |f| f << "X" << "Y" }
p File.read(T)
f = File.open(T, "r")
p f.closed?
p f.read.length > 0
f.close
p f.closed?
File.delete(T)
p $stdout.class
p STDOUT.class
$stdout.puts "to-stdout"
STDOUT.write("via-write\n")
p $stdout.class == STDOUT.class
