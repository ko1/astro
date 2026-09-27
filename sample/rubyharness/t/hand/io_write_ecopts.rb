# A transcoding stream's open options invalid:/undef:/replace: apply to its writes
# (write, <<, print, puts), and a failed conversion raises String#encode's error
# ("\x82" on UTF-8 with error_bytes and the encoding names).  csv; w12 A13.
require "tmpdir"; require "fileutils"
dir = Dir.mktmpdir("ecw"); path = File.join(dir, "e.tmp")
File.open(path, "w", encoding: Encoding::CP932) do |f|
  p((f.write("\x82\xa0") rescue [$!.class, $!.message, $!.error_bytes, $!.source_encoding_name, $!.destination_encoding_name]))
  p((f << "·" rescue [$!.class, $!.message]))
  f.write("あ")
end
p File.binread(path)
File.open(path, "w", encoding: Encoding::CP932, invalid: :replace) { |f| p f.write("\x82\xa0"); f.print("x\x82") }
p File.binread(path)
File.open(path, "w", encoding: Encoding::CP932, undef: :replace) { |f| p f.write("·"); f.puts("·a") }
p File.binread(path)
File.open(path, "w", encoding: Encoding::CP932, undef: :replace, replace: "*") { |f| f << "·" << "い" }
p File.binread(path)
File.open(path, "w", encoding: Encoding::CP932, invalid: nil) { |f| p((f.write("\x82") rescue $!.class)) }
FileUtils.rm_rf(dir)
