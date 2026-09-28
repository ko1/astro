require 'tmpdir'
Dir.mktmpdir do |d|
  a = File.join(d, "log"); File.write(a, "x")
  f = File.open(a, "a")
  i0 = f.stat.ino
  File.rename(a, a + ".0"); File.write(a, "y")
  p f.stat.ino == i0, File.stat(a + ".0").ino == i0, (File.identical?(a + ".0", f)), File.identical?(a, f), File.identical?(f, f)
  f.close
end
