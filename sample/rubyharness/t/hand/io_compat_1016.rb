# IO#lineno= negative, gets(limit) completes up to 16 bytes, paragraph chomp keeps the last
# "\n", IO.readlines default encodings, ungetc(codepoint), set_encoding ecopts, IO.pipe and
# a subclass #initialize (2026-10-16)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
dir = File.join(__dir__, "io_tmp_#{$$}")
Dir.mkdir(dir)
begin
  f = File.join(dir, "f.txt")
  File.write(f, "朝日" + ([227, 129].pack("C*") * 20).force_encoding("utf-8"))
  t(:gets_limit) { File.open(f, "r:utf-8") { |io| io.gets(7).bytesize } }
  t(:lineno_neg) { File.open(f) { |io| io.lineno = -5; [io.lineno, $.] } }
  pf = File.join(dir, "p.txt")
  File.write(pf, "a\n\nb\n")
  t(:para_chomp) { File.open(pf) { |io| io.each_line("", chomp: true).to_a } }
  t(:readlines_enc) {
    ext, int = Encoding.default_external, Encoding.default_internal
    $VERBOSE, v = nil, $VERBOSE
    begin
      Encoding.default_external = Encoding::BINARY; Encoding.default_internal = Encoding::UTF_8
      a = IO.readlines(pf).map(&:encoding).uniq
      Encoding.default_external = Encoding::UTF_8; Encoding.default_internal = Encoding::UTF_16
      b = IO.readlines(pf).map(&:encoding).uniq
      [a, b]
    ensure
      Encoding.default_external = ext; Encoding.default_internal = int; $VERBOSE = v
    end
  }
  t(:ungetc_cp) { File.open(pf) { |io| io.set_encoding(Encoding::UTF_8); io.ungetc(233); c = io.getc; [c, c.bytes] } }
  ff = File.join(dir, "ff.txt")
  File.binwrite(ff, "\xff")
  t(:set_enc_opts) { File.open(ff) { |io| io.set_encoding(Encoding::EUC_JP, Encoding::SHIFT_JIS, invalid: :replace, replace: "."); io.read } }
  t(:open_enc_opts) { File.open(ff, "r:EUC-JP:Shift_JIS", invalid: :replace, replace: "?") { |io| io.read } }
  $calls = []
  class SubPipeIO < IO
    def self.new(...) = ($calls << :new; super)
    def initialize(...) = ($calls << :init; super)
  end
  t(:pipe_sub) { r, w = SubPipeIO.pipe; w.write "hi"; w.close; [r.read, $calls, r.class] }
ensure
  Dir.glob(File.join(dir, "*")).each { |x| File.unlink(x) }
  Dir.rmdir(dir)
end
