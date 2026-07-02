# Dir.mkdir/entries/children/glob/[]/chdir/rmdir + $$ (deterministic). vs ruby.
D = "/tmp/koruby_dircorpus"
Dir.children(D).each { |f| File.delete("#{D}/#{f}") } if Dir.exist?(D)  # clean leftovers
Dir.rmdir(D) if Dir.exist?(D)
Dir.mkdir(D)
File.write("#{D}/a.txt", "1"); File.write("#{D}/b.txt", "2"); File.write("#{D}/c.log", "3")
p Dir.entries(D).sort
p Dir.children(D).sort
p Dir.glob("#{D}/*.txt").sort.map { |f| File.basename(f) }
p Dir["#{D}/*.log"].map { |f| File.basename(f) }
Dir.chdir(D) { p File.basename(Dir.pwd) }
p Dir.exist?(D)
File.delete("#{D}/a.txt", "#{D}/b.txt", "#{D}/c.log")
Dir.rmdir(D)
p Dir.exist?(D)
p ($$.is_a?(Integer) && $$ > 0)
