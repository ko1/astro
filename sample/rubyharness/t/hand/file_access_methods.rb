# File.readable?/writable?/executable?/chmod/umask. vs ruby.
f = "/tmp/koruby-facc-test.txt"
File.write(f, "hi")
p File.readable?(f)
p File.writable?(f)
p File.readable?("/nonexistent-koruby-xyz")
p File.chmod(0644, f)
p File.executable?(f)
File.chmod(0755, f)
p File.executable?(f)
p File.umask.class
File.delete(f)
p File.readable?(f)
