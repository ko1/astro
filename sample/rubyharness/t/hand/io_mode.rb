# File.open mode enforcement: IOError on wrong-direction I/O, ArgumentError on a
# bad mode, integer O_* flags. vs ruby.
f = "/tmp/koruby-io-mode-fixture.txt"
File.write(f, "data")
r = File.open(f, "r")
begin; r.write("x"); rescue IOError => e; p e.message; end
p r.read
r.close
w = File.open(f, "w")
begin; w.read; rescue IOError => e; p e.message; end
w.write("new"); w.close
p File.read(f)
begin; File.open(f, "zzz"); rescue ArgumentError; p :badmode; end
wc = File.open(f, File::WRONLY | File::CREAT, 0644)
wc.write("!"); wc.close
p File.read(f)
rw = File.open(f, "r+")
p rw.read
rw.write("Z")
rw.close
File.delete(f)
