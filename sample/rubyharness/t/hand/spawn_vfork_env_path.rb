# system / spawn / IO.popen start the child with vfork: the environment is built
# before (set / unset / unsetenv_others), PATH is searched before, a script
# without #! still runs via /bin/sh, and errors are reported as before.
require "tmpdir"
dir = Dir.mktmpdir("spawnv")
File.write("#{dir}/noshebang", "echo from-noshebang $1\n"); File.chmod(0755, "#{dir}/noshebang")
File.write("#{dir}/notexec", "echo x\n"); File.chmod(0644, "#{dir}/notexec")
ENV["KORB_SPAWN_T"] = "parent"
p IO.popen(["sh", "-c", "echo $KORB_SPAWN_T"], &:read)
p IO.popen([{ "KORB_SPAWN_T" => "child" }, "sh", "-c", "echo $KORB_SPAWN_T"], &:read)
p IO.popen([{ "KORB_SPAWN_T" => nil }, "sh", "-c", "echo ${KORB_SPAWN_T-unset}"], &:read)
p IO.popen([{ "ONLY" => "1" }, "/usr/bin/env"], unsetenv_others: true, &:read)
p ENV["KORB_SPAWN_T"]
p IO.popen(["pwd"], chdir: dir, &:read).strip == File.realpath(dir)
p IO.popen([{ "PATH" => "#{dir}:#{ENV["PATH"]}" }, "noshebang", "arg1"], &:read)
p system("#{dir}/noshebang", "a2", out: File::NULL), $?.exitstatus
p (system("definitely-not-a-command-xyz", exception: true) rescue $!.class)
p system("definitely-not-a-command-xyz"), $?.exitstatus
p (spawn("#{dir}/notexec") rescue $!.class)
p (spawn({ "PATH" => dir }, "notexec") rescue $!.class)
p (spawn("true", chdir: "#{dir}/missing") rescue [$!.class, $!.message.include?("missing")])
r, w = IO.pipe
pid = spawn("sh", "-c", "echo out; echo err 1>&2", out: w, err: [:child, :out]); w.close
Process.wait(pid); p r.read, $?.success?
p `echo backtick`, $?.exitstatus
pid = spawn("sh", "-c", "exit 3"); Process.wait(pid); p $?.exitstatus
p 20.times.map { IO.popen(["echo", "x"], &:read) }.uniq
p IO.popen("-") { |io| io ? io.read : (print "forked"; exit!(0)) }
FileUtils.rm_rf(dir) if defined?(FileUtils)
require "fileutils"; FileUtils.rm_rf(dir)
