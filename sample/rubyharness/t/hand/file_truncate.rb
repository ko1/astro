# File.truncate / File#truncate / File.absolute_path? / File.absolute_path. vs ruby.
f = "/tmp/koruby-file-truncate-fixture.txt"
File.write(f, "0123456789")
File.truncate(f, 5)
p File.read(f)
p File.size(f)
io = File.open(f, "r+")
io.truncate(3)
io.close
p File.read(f)
File.truncate(f, 0)
p File.read(f)
p File.absolute_path?("/abs")
p File.absolute_path?("rel")
p File.absolute_path("x").start_with?("/")
File.delete(f)
