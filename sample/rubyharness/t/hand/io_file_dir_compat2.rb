# IO / File / Dir / ARGF details from rubyspec: buffers, encodings, block forms,
# flock, enumerator sizes, to_io coercion. vs ruby.
require "tmpdir"
require "stringio"
$stdout.sync = true
def try; yield; rescue Exception => e; [e.class, e.message]; end

Dir.mktmpdir do |d|
  f = File.join(d, "a.txt"); File.write(f, "Hello\nworld\n")

  # IO.binread offset, File.read + default_external, sysread / pread buffers
  p try { IO.binread(f, 0, -1) }.first, try { IO.read(f, 0, -1) }.first
  Encoding.default_external = Encoding::US_ASCII
  p File.read(f).encoding, IO.read(f).encoding, File.binread(f).encoding
  Encoding.default_external = Encoding::UTF_8
  File.open(f) do |io|
    buf = "".encode(Encoding::ISO_8859_1)
    io.sysread(3, buf); p buf, buf.encoding
    buf2 = +"existing"; p try { io.pread(1, 100, buf2) }.first, buf2
    p try { io.sysseek(-2, IO::SEEK_CUR) }
  end

  # File.open(fd) with a block; File.new ignores a block (with a warning)
  fd = IO.sysopen(f, "w")
  r = File.open(fd, "w") { |ff| ff.write("via fd"); ff }
  p r.class, r.closed?, File.read(f)
  w = nil
  begin
    old = $stderr; $stderr = StringIO.new rescue nil
    fh = File.new(f) { raise "block ran" }
    w = $stderr.string if $stderr.respond_to?(:string)
  ensure
    $stderr = old
  end
  p fh.class, w.to_s.include?("File::new() does not take block; use File::open() instead")
  fh.close

  # IO.open swallows only the "closed stream" IOError from the ensure-close
  klass = Class.new(IO) { def close; raise IOError, "closed stream"; end }
  rd, wr = IO.pipe
  p klass.open(wr.fileno, "w") { |io| :body }, $!
  klass2 = Class.new(IO) { def close; raise IOError, "other"; end }
  p try { klass2.open(rd.fileno) { |io| :body } }

  # flush / close on a pipe whose reader went away
  rr, ww = IO.pipe
  ww.sync = false; ww.write "foo"; rr.close
  p try { ww.flush }.first, try { ww.close }.first

  # reopen on a closed stream asks #to_io first
  io = File.open(f); io.close
  o = Object.new; def o.to_io; $stdout; end
  p try { io.reopen(o) }.first

  # IO.select with #to_io objects; non-IO is a TypeError
  rr, ww = IO.pipe; ww.write("x")
  o = Object.new; o.define_singleton_method(:to_io) { rr }
  res = IO.select([o], nil, nil, 1); p res[0][0].equal?(o), res[1], res[2]
  p try { IO.select([Object.new]) }.first, try { IO.select(nil, [Object.new]) }.first
  rr.close; ww.close

  # each_char / Dir#each enumerators; closed each_char raises when iterated
  p File.open(f) { |io| io.each_char.size }
  c = File.open(f); c.close; p try { c.each_char.first }.first
  dd = Dir.new(d); p dd.each.size, dd.each.to_a.sort; dd.close
  p Dir.new(d.dup.force_encoding("IBM866")).to_path.encoding

  # copy_stream with read/readpartial objects and an unreadable source
  src = Class.new { def initialize(io); @io = io; end; def readpartial(n, buf); @io.readpartial(n, buf); end }
  src2 = Class.new { def initialize(io); @io = io; end; def read(n, buf); @io.read(n, buf); end }
  out = File.join(d, "out.txt")
  File.open(f) { |io| IO.copy_stream(src.new(io), out) }; p File.read(out)
  File.open(f) { |io| IO.copy_stream(src2.new(io), out) }; p File.read(out)
  File.open(f, "a") { |io| p try { IO.copy_stream(io, out) }.first }

  # IO::Buffer.string / #write
  p IO::Buffer.string(7) { |b| b.set_string("ä test") }.encoding
  IO::Buffer.for("Hello") { |b| File.open(out, "w") { |io| p b.write(io, 0), b.write(io, 0, 1) } }
  p File.read(out)

  # File predicates: #to_io objects, fnmatch arity, grpowned?, birthtime on /proc
  File.open(d) { |dio| o = Object.new; o.define_singleton_method(:to_io) { dio }; p File.directory?(o) }
  p try { File.fnmatch(nil, nil, 0, 0) }.first, try { File.fnmatch(1, "x") }.first
  p File.grpowned?(f), File.stat(f).grpowned?
  begin
    p File.birthtime("/proc").class
  rescue NotImplementedError => e
    p e.message
  end if File.directory?("/proc")

  # File#flock: LOCK_NB answers false when another descriptor holds the lock
  File.open(f, "w+") do |a|
    p a.flock(File::LOCK_EX), a.flock(File::LOCK_UN), a.flock(File::LOCK_SH)
    a.flock(File::LOCK_EX)
    File.open(f, "w") { |b| p b.flock(File::LOCK_EX | File::LOCK_NB) }
    a.flock(File::LOCK_UN)
    File.open(f, "w") { |b| p b.flock(File::LOCK_EX | File::LOCK_NB) }
  end

  # Dir.chdir { } raises when the original directory vanished
  d1 = File.join(d, "d1"); d2 = File.join(d, "d2"); Dir.mkdir(d1); Dir.mkdir(d2)
  here = Dir.pwd
  p try { Dir.chdir(d1) { Dir.chdir(d2) { Dir.rmdir(d1) } } }.first
  Dir.chdir(here)

  # ARGF: argv identity, read encoding, stdin via $stdin
  p ARGF.argv.equal?(ARGV)
  Encoding.default_external = Encoding::US_ASCII
  p ARGF.class.new(f).read.encoding
  Encoding.default_external = Encoding::UTF_8
  p ARGF.class.new(f).tap(&:binmode).read.encoding
  rr, ww = IO.pipe
  saved = $stdin; $stdin = rr
  p ARGF.class.new.read_nonblock(4, exception: false)
  ww.write("abcd"); p ARGF.class.new.read_nonblock(4)
  $stdin = saved; rr.close; ww.close
end
