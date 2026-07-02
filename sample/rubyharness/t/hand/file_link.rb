# File.link / File.symlink / File.readlink. vs ruby. Self-contained.
d = "/tmp/koruby-file-link-fixture"
Dir.rmdir(d) if Dir.exist?(d) && Dir.children(d).empty?
["orig","hard","sym"].each { |f| p2 = "#{d}/#{f}"; File.delete(p2) if File.exist?(p2) }
Dir.rmdir(d) if Dir.exist?(d)
Dir.mkdir(d)
File.write("#{d}/orig", "hello")
File.link("#{d}/orig", "#{d}/hard")
p File.read("#{d}/hard")
File.symlink("#{d}/orig", "#{d}/sym")
p File.readlink("#{d}/sym")
p File.read("#{d}/sym")
begin; File.readlink("#{d}/orig"); rescue SystemCallError; p :notlink; end
["orig","hard","sym"].each { |f| File.delete("#{d}/#{f}") }
Dir.rmdir(d)
