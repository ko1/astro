# Line reading from a pipe copies only up to the separator; a separator that
# straddles the 8 KiB read buffer, multi-byte separators, paragraph mode, a
# limit and chomp all still split the same way.
def via_pipe(data)
  r, w = IO.pipe
  t = Thread.new { w.write(data); w.close }
  v = yield r
  t.join; r.close
  v
end
[8189, 8190, 8191, 8192, 8193].each do |off|
  data = "a" * off + "XYZ" + "b" * 10 + "XYZ" + "c"
  p via_pipe(data) { |r| r.each_line("XYZ").map(&:size) }
  p via_pipe("a" * off + "\r\n" + "b\r\n") { |r| r.each_line("\r\n").map(&:size) }
  p via_pipe("a" * off + "\n" + "b" * 3) { |r| l = []; while (x = r.gets); l << x.size; end; l }
end
p via_pipe("a\nb\n\n\nc\n\nd") { |r| r.each_line("").to_a }
p via_pipe("ab\ncd\nef") { |r| r.each_line(3).to_a }
p via_pipe("ab\ncd\nef") { |r| r.each_line(chomp: true).to_a }
p via_pipe("x\n" * 20000) { |r| r.each_line.count }
p via_pipe("é\nü" * 3000) { |r| r.readlines.map(&:bytesize).uniq }
