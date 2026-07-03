# File.new(path[, mode]) opens and returns a File (no block-close). vs ruby.
f = "/tmp/koruby-file-new-fixture.txt"
File.write(f, "hello")
io = File.new(f)
p io.class.to_s
p io.read
io.close
io2 = File.new(f, "r")
p io2.read
io2.close
w = File.new(f, "w")
w.write("bye")
w.close
p File.read(f)
File.delete(f)
