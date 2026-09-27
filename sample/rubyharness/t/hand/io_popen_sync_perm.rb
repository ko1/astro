require 'tmpdir'

# popen streams are sync; a failed sync write leaves nothing for #close to re-raise
IO.popen("cat >/dev/null", "w") {|f| p f.sync }
r, w = IO.pipe
p w.sync
r.close; w.close
pr = proc do |f|
  sleep 0.2
  f.puts "x"
rescue Errno::EPIPE
  p :rescued
end
IO.popen("true", "w", &pr)
p :after

Dir.mktmpdir do |dir|
  # File.write honours perm: for a new file, and keeps an existing file's mode
  path = File.join(dir, "cmd")
  File.write(path, "#!/bin/sh\necho ran\n", perm: 0o700)
  printf("%o\n", File.stat(path).mode & 0o777)
  File.write(path, "#!/bin/sh\necho ran again\n", perm: 0o600)
  printf("%o\n", File.stat(path).mode & 0o777)
  IO.popen(path) {|f| p f.read }

  # a command that cannot be executed raises, with $? set
  ne = File.join(dir, "ne")
  File.write(ne, "echo hi\n", perm: 0o644)
  p((IO.popen(ne) {} rescue $!.class))
  p $?.exitstatus
  p((IO.popen(["nonexist_cmd_zz"]) {} rescue $!.class))
  p IO.popen("exit 3") {|f| f.read }, $?.exitstatus
end
