# Dir.glob: ** recursion, ? / [] / {} wildcards, array patterns, block form. vs ruby.
D = "/tmp/koruby_globcorpus"
Dir.children(D).each { |f| ff="#{D}/#{f}"; File.directory?(ff) ? (Dir.children(ff).each{|g| File.delete("#{ff}/#{g}")}; Dir.rmdir(ff)) : File.delete(ff) } if Dir.exist?(D)
Dir.rmdir(D) if Dir.exist?(D)
Dir.mkdir(D); Dir.mkdir("#{D}/sub")
File.write("#{D}/a.txt","1"); File.write("#{D}/b.txt","2"); File.write("#{D}/c.md","3")
File.write("#{D}/sub/d.txt","4"); File.write("#{D}/sub/e.md","5")
p Dir.glob("#{D}/*.txt").map { |f| File.basename(f) }.sort
p Dir.glob("#{D}/**/*.txt").map { |f| File.basename(f) }.sort
p Dir.glob("#{D}/?.txt").map { |f| File.basename(f) }.sort
p Dir.glob("#{D}/*.{txt,md}").map { |f| File.basename(f) }.sort
p Dir.glob("#{D}/[ab].txt").map { |f| File.basename(f) }.sort
p Dir.glob(["#{D}/*.txt","#{D}/*.md"]).map { |f| File.basename(f) }.sort
m = []; Dir.glob("#{D}/*.txt") { |f| m << File.basename(f) }; p m.sort
p Dir["#{D}/**/*.md"].map { |f| File.basename(f) }.sort
Dir.children(D).each { |f| ff="#{D}/#{f}"; File.directory?(ff) ? (Dir.children(ff).each{|g| File.delete("#{ff}/#{g}")}; Dir.rmdir(ff)) : File.delete(ff) }
Dir.rmdir(D)
