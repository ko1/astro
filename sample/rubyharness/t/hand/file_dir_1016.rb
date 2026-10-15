# File.realpath keeps the path's encoding, a binary path's errno message, expand_path under a
# wide external encoding, Dir.for_fd and closedir EBADF (2026-10-16)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
dir = File.join(__dir__, "rp_tmp_あ_#{$$}")
Dir.mkdir(dir)
begin
  t(:realpath_enc) { [File.realpath(".".encode(Encoding::ISO_8859_1), dir).encoding, File.realpath(".", dir).encoding,
                      File.realdirpath(".".encode(Encoding::ISO_8859_1), dir).encoding] }
  t(:stat_binary) { begin; File.stat("/missingfilepath\xE3E4".b); rescue SystemCallError => e; [e.class, e.message.include?("/missingfilepath\xE3E4".b)]; end }
  t(:expand_wide) {
    ext = Encoding.default_external
    begin; Encoding.default_external = Encoding::UTF_16BE; File.expand_path("./a"); ensure; Encoding.default_external = ext; end
  }
  t(:for_fd) {
    d = Dir.open(dir); n = Dir.for_fd(d.fileno)
    r = [n.class, n.fileno == d.fileno]
    d.close
    begin; n.close; rescue SystemCallError => e; r << e.class << e.message; end
    r
  }
ensure
  Dir.rmdir(dir)
end
