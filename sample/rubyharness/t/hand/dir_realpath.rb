# Kernel#__dir__ + File.realpath/realdirpath. vs ruby. Self-contained.
f = "/tmp/koruby-dr-#{$$}.txt"; File.write(f, "x")
p File.realpath(f) == f
p File.realpath("#{File.basename(f)}", "/tmp") == f
p __dir__.is_a?(String)
p __dir__ == File.dirname(File.realpath(__FILE__))
begin; File.realpath("/no/such/path/xyz"); rescue Errno::ENOENT; p :enoent; end
File.delete(f)
