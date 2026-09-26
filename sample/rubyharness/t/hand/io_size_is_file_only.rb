# #size and the file times are File's, not IO's.
p [:size, :atime, :ctime, :mtime, :lstat].map { IO.method_defined?(_1) }, [:size, :atime, :mtime, :lstat].map { File.method_defined?(_1) }
r, w = IO.pipe; p r.respond_to?(:size), $stdout.respond_to?(:mtime)
File.write((path = "/tmp/kz_ios_#{$$}.txt"), "abc"); File.open((path = "/tmp/kz_ios_#{$$}.txt")) { |f| p f.size, f.mtime.class, f.lstat.size }
File.delete(path)
