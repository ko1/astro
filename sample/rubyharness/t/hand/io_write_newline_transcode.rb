# Write-side newline decorators (newline: :crlf/:cr/:lf and the *_newline: flags) apply
# to write / print / << / puts / putc / printf / File.write, before transcoding; print,
# puts and << transcode into the external encoding like #write (csv; w12 A12 / A14).
require "tmpdir"; require "fileutils"
DIR = Dir.mktmpdir("wnl"); at_exit { FileUtils.rm_rf(DIR) }
PATH = File.join(DIR, "w.tmp")
def show(label)
  r = begin
    yield
  rescue Exception => e
    "#{e.class}: #{e.message}"
  end
  puts "#{label}: #{r.inspect}"
end

puts "== write side (newline decorators; read-side options are no-ops here)"
[["w", {}], ["wt", {}], ["w", {newline: :crlf}], ["w", {newline: :cr}], 
 ["w", {newline: :universal}], ["w", {crlf_newline: true}], ["w", {cr_newline: true}], 
 ["w", {universal_newline: true}], ["w", {textmode: true}], ["w:UTF-16LE", {newline: :crlf}],
 ["wb", {newline: :crlf}], ["wb", {universal_newline: false}]].each do |m, kw|
  show("write #{m} #{kw}") do
    n = File.open(PATH, m, **kw) { |f| x = f.write("a\nb\r\nc\r"); f.print("d\n"); f.puts("e"); f << "f\n"; f.putc "\n"; f.printf("%s\n", "g"); x }
    [n, File.binread(PATH)]
  end
end
show("File.write newline crlf") { File.write(PATH, "x\ny\n", newline: :crlf); File.binread(PATH) }
show("File.write crlf_newline") { File.write(PATH, "x\ny\n", mode: "w", crlf_newline: true); File.binread(PATH) }
show("r+t write raw") { File.binwrite(PATH, "q\r\n"); File.open(PATH, "r+t") { |f| [f.gets, f.write("z\n")] }; File.binread(PATH) }
path = File.join(DIR, "t94.tmp")
File.open(path, "wb", encoding: "UTF-16LE") { |f| f << "abc" }
p File.binread(path).bytes
File.open(path, "w", encoding: "UTF-16LE") { |f| f << "abc" }
p File.binread(path).bytes
File.open(path, "w", encoding: "UTF-16LE") { |f| f.print "a\n"; f.puts "b"; f.printf("%d", 1) }; p File.binread(path).bytes
File.open(path, "w", newline: :lf) { |f| f.write("a\r\nb") }; p File.binread(path)
