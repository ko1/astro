# Dir instance objects: new/open/read/each/pos/rewind/path/children/close. vs
# ruby. Self-contained: builds a known dir tree in /tmp.
base = "/tmp/koruby-dir-instance-fixture"
files = %w[a.txt b.txt c.txt]
files.each { |n| p = "#{base}/#{n}"; File.delete(p) if File.exist?(p) }
Dir.rmdir(base) if Dir.exist?(base)
Dir.mkdir(base)
files.each { |n| File.write("#{base}/#{n}", "x") }
d = Dir.new(base)
p d.path
p d.class.to_s
all = []
while (e = d.read); all << e; end
p all.sort
p all.include?(".") && all.include?("..")
d.rewind
p d.pos
p d.read.is_a?(String)
d.close
names = []
Dir.open(base) { |dir| dir.each { |n| names << n } }
p names.sort
p Dir.open(base) { |dir| dir.children.sort }
p(Dir.open(base) { :blockval })
files.each { |n| File.delete("#{base}/#{n}") }
Dir.rmdir(base)
