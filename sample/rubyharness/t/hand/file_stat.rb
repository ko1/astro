# File.stat / File.lstat / IO#stat → File::Stat. vs ruby. Self-contained.
f = "/tmp/koruby-file-stat-fixture.txt"
File.write(f, "0123456789")
st = File.stat(f)
p st.class.to_s
p st.is_a?(File::Stat)
p st.size
p st.file?
p st.directory?
p st.symlink?
p st.zero?
p st.ftype
p st.mode.is_a?(Integer)
p st.nlink >= 1
p st.uid.is_a?(Integer)
p st.mtime.class.to_s
p st.blksize.is_a?(Integer)
d = File.stat("/tmp")
p d.directory?
p d.ftype
io = File.open(f, "r")
p io.stat.size
p io.fileno.is_a?(Integer)
io.close
p (File.stat(f) <=> File.stat(f))
File.delete(f)
