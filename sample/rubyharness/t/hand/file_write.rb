# File.write / readlines / foreach / delete round-trips. vs ruby.
T = "/tmp/koruby_fwtest_#{$$}"
p File.write(T, "hello\nworld\n")
p File.read(T)
File.write(T, "appended\n", mode: "a")
p File.read(T)
p File.readlines(T)
collected = []
File.foreach(T) { |l| collected << l.chomp }
p collected
p File.write(T, "xyz")
p File.read(T)
p File.exist?(T)
p File.delete(T)
p File.exist?(T)
begin; File.delete(T); rescue => e; p :raised_on_missing; end
