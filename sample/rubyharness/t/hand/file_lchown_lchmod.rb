# File.lchown / File.lchmod (fileutils cp_r preserve on symlinks): lchmod on a Linux
# symlink is ENOTSUP as in CRuby; the chown/chmod family report "@ apply2files".
require "tmpdir"
Dir.mktmpdir("lch") do |d|
  f = "#{d}/f"; File.write(f, "x"); l = "#{d}/l"; File.symlink(f, l); dl = "#{d}/dl"; File.symlink("#{d}/none", dl)
  %i[lchown lutime lchmod lstat].each { |m| p [m, File.respond_to?(m)] }
  p File.lchown(nil, nil, l, dl), File.lchown(-1, -1, l)
  p (File.lchmod(0600, dl) rescue [$!.class, $!.message.sub(d, "D")])
  p File.lchmod(0600, f), File.stat(f).mode.to_s(8)
  p (File.lchown(nil, nil, "#{d}/missing") rescue [$!.class, $!.message.sub(d, "D")])
  p (File.chown(nil, nil, "#{d}/missing") rescue [$!.class, $!.message.sub(d, "D")])
  p (File.chmod(0644, "#{d}/missing") rescue [$!.class, $!.message.sub(d, "D")])
end
