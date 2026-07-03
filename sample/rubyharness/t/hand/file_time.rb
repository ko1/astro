# File.atime / ctime / mtime class methods → Time (via stat). vs ruby.
f = "/tmp/koruby-file-time-fixture.txt"
File.write(f, "content")
p File.mtime(f).class.to_s
p File.atime(f).class.to_s
p File.ctime(f).class.to_s
p File.mtime(f) == File.stat(f).mtime
p File.mtime(f).is_a?(Time)
begin; File.mtime("/no/such/path"); rescue Errno::ENOENT; p :enoent; end
File.delete(f)
