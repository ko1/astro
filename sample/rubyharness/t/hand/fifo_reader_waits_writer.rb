# A read-only FIFO opened before any writer: the read waits for the writer
# instead of reporting EOF (rubyspec core/file/open_spec under load).
require 'tmpdir'
Dir.mktmpdir do |d|
  f = File.join(d, "fifo")
  File.mkfifo(f)
  %i[sysread read gets].each do |m|
    r = Thread.new { io = File.open(f, "r"); v = m == :gets ? io.gets : io.public_send(m, 5); io.close; v }
    w = Thread.new { sleep 0.1; io = File.open(f, "w"); io.syswrite("hello\n"); io.close }
    w.join
    p [m, r.value]
  end
  r = Thread.new { io = File.open(f, "r"); v = io.read; io.close; v }
  w = Thread.new { sleep 0.1; File.open(f, "w").close }
  w.join
  p r.value
end
