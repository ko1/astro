# StringIO — an in-memory IO-alike backed by a String buffer + read position.
# Pure Ruby (the real stdlib StringIO is C); covers the read/write API incl.
# mode tracking (closed_read?/closed_write?), pos-aware writes, ungetc,
# sysread/readpartial/nonblock variants and lineno.
class StringIO
  include Enumerable

  def initialize(string = +"", mode = nil, **opts)
    @buf = string
    mode = opts[:mode] if mode.nil? && opts.key?(:mode)   # StringIO.new(str, mode: "r")
    mode ||= string.frozen? ? "r" : "r+"
    mode = mode.to_s
    mode += "b" if opts[:binmode] && !mode.include?("b")
    @append = mode.start_with?("a")
    plus = mode.include?("+")
    @readable = plus || mode.start_with?("r")
    @writable = plus || mode.start_with?("w") || @append
    @buf = @buf.dup if @buf.frozen? && @writable
    @buf.clear if mode.start_with?("w")
    @pos = 0
    @lineno = 0
    @closed_read = false
    @closed_write = false
  end

  def string; @buf; end
  def string=(s); @buf = s; @pos = 0; @lineno = 0; s; end
  def to_s;   @buf; end
  def size;   @buf.bytesize; end
  alias length size
  def pos;    @pos; end
  def pos=(n); @pos = n; end
  alias tell pos
  def rewind;  @pos = 0; @lineno = 0; 0; end
  def eof?;    @pos >= @buf.bytesize; end
  alias eof eof?
  def lineno; @lineno; end
  def lineno=(n); @lineno = n; end
  def fileno; nil; end
  def pid; nil; end
  def tty?; false; end
  alias isatty tty?
  def flush; self; end
  def fsync; 0; end
  def sync; true; end
  def sync=(v); v; end
  def binmode; self; end
  def internal_encoding; nil; end
  def external_encoding; @buf.encoding; end

  def close; @closed_read = true; @closed_write = true; nil; end
  def close_read
    raise IOError, "closing non-duplex IO for reading" unless @readable
    @closed_read = true
    nil
  end
  def close_write
    raise IOError, "closing non-duplex IO for writing" unless @writable
    @closed_write = true
    nil
  end
  def closed?; (!@readable || @closed_read) && (!@writable || @closed_write); end
  def closed_read?;  !@readable || @closed_read; end
  def closed_write?; !@writable || @closed_write; end

  def reopen(other = +"", mode = nil)
    if other.is_a?(StringIO)
      @buf = other.string
      @pos = other.pos
      @readable = !other.closed_read?
      @writable = !other.closed_write?
      @append = false
    else
      other = other.to_str unless other.is_a?(String)
      initialize(other, mode || (other.frozen? ? "r" : "r+"))
    end
    @closed_read = false
    @closed_write = false
    @lineno = 0
    self
  end

  def __check_readable
    raise IOError, "not opened for reading" if closed_read?
  end
  def __check_writable
    raise IOError, "not opened for writing" if closed_write?
  end
  private :__check_readable, :__check_writable

  def seek(amount, whence = 0)
    case whence
    when 1 then @pos += amount
    when 2 then @pos = @buf.bytesize + amount
    else        @pos = amount
    end
    raise Errno::EINVAL, "Invalid argument" if @pos < 0
    0
  end

  # ---- writing (pos-aware; append mode writes at the end) -------------------
  def write(*args)
    __check_writable
    n = 0
    args.each do |a|
      s = a.is_a?(String) ? a : a.to_s
      next if s.empty?
      if @append
        @buf << s
        @pos = @buf.bytesize
      else
        @buf << ("\0" * (@pos - @buf.bytesize)) if @pos > @buf.bytesize   # sparse pad
        @buf[@pos, s.bytesize] = s
        @pos += s.bytesize
      end
      n += s.bytesize
    end
    n
  end
  def syswrite(s); write(s); end
  def write_nonblock(s, exception: true); write(s); end

  def <<(obj)
    write(obj)
    self
  end

  def print(*args)
    args.each { |a| write(a.nil? ? "" : a) }
    write($\) if $\
    nil
  end

  def printf(fmt, *args)
    write(sprintf(fmt, *args))
    nil
  end

  def putc(ch)
    __check_writable
    write(ch.is_a?(Integer) ? (ch & 0xff).chr : ch.to_s[0])
    ch
  end

  def puts(*args)
    if args.empty?
      write("\n")
    else
      args.each do |a|
        if a.is_a?(Array)
          puts(*a)
        else
          s = a.to_s
          write(s)
          write("\n") unless s.end_with?("\n")
        end
      end
    end
    nil
  end

  def truncate(n)
    __check_writable
    raise Errno::EINVAL, "negative length" if n < 0
    cur = @buf.bytesize
    if n < cur
      @buf.slice!(n, cur - n)
    elsif n > cur
      @buf << ("\0" * (n - cur))
    end
    0
  end

  # ---- reading --------------------------------------------------------------
  def read(length = nil, outbuf = nil)
    __check_readable
    if length
      raise ArgumentError, "negative length #{length} given" if length < 0
      return (length == 0 ? (outbuf ? outbuf.replace("") : "") : nil) if eof?
      r = @buf.byteslice(@pos, length) || ""
    else
      r = @buf.byteslice(@pos, @buf.bytesize - @pos) || ""
    end
    @pos += r.bytesize
    outbuf ? outbuf.replace(r) : r
  end

  def sysread(length = nil, outbuf = nil)
    raise EOFError, "end of file reached" if eof?
    read(length, outbuf)
  end
  def readpartial(length = nil, outbuf = nil); sysread(length, outbuf); end
  def read_nonblock(length = nil, outbuf = nil, exception: true)
    if eof? && !exception
      return nil
    end
    sysread(length, outbuf)
  end

  def getc
    __check_readable
    return nil if eof?
    # @pos is a BYTE offset; String#[] indexes by character, so slice by bytes
    # and take the first character of the slice (max 4 bytes covers UTF-8).
    ch = @buf.byteslice(@pos, 4).to_s[0]
    return nil if ch.nil?
    @pos += ch.bytesize
    ch
  end

  def readchar
    c = getc
    raise EOFError, "end of file reached" if c.nil?
    c
  end

  def readbyte
    b = getbyte
    raise EOFError, "end of file reached" if b.nil?
    b
  end

  def getbyte
    __check_readable
    return nil if eof?
    b = @buf.getbyte(@pos)
    @pos += 1
    b
  end

  def ungetc(ch)
    __check_readable
    s = ch.is_a?(Integer) ? ch.chr : ch.to_s
    if @pos >= s.bytesize
      @pos -= s.bytesize
      @buf[@pos, s.bytesize] = s
    else
      @buf[0, @pos] = s
      @pos = 0
    end
    nil
  end
  def ungetbyte(b)
    ungetc(b.is_a?(Integer) ? (b & 0xff).chr : b)
  end

  def gets(sep = $/, limit = nil, chomp: false)
    __check_readable
    if sep.is_a?(Integer) && limit.nil?
      limit = sep
      sep = $/
    elsif !sep.nil? && !sep.is_a?(String)
      raise TypeError, "no implicit conversion of #{sep.class} into String" unless sep.respond_to?(:to_str)
      sep = sep.to_str
    end
    return nil if eof?
    if sep.nil?
      line = read(limit)
    else
      sep = "\n\n" if sep == ""                        # paragraph mode (簡易)
      rest = @buf.byteslice(@pos, @buf.bytesize - @pos)
      i = rest.index(sep)
      line = i ? rest[0, i + sep.length] : rest
      line = line[0, limit] if limit && line.length > limit
      @pos += line.bytesize
    end
    @lineno += 1
    chomp ? line.chomp : line
  end

  def readline(sep = $/, limit = nil, chomp: false)
    raise EOFError, "end of file reached" if eof?
    gets(sep, limit, chomp: chomp)
  end

  def each_line(sep = $/, limit = nil, chomp: false)
    return to_enum(:each_line, sep, limit, chomp: chomp) unless block_given?
    sep, limit = $/, sep if sep.is_a?(Integer) && limit.nil?   # each_line(10) form
    while (l = gets(sep, limit, chomp: chomp))
      yield l
    end
    self
  end
  alias each each_line

  def readlines(sep = $/, limit = nil, chomp: false)
    ls = []
    each_line(sep, limit, chomp: chomp) { |l| ls << l }
    ls
  end

  def each_char
    return to_enum(:each_char) unless block_given?
    while (c = getc)
      yield c
    end
    self
  end

  def each_byte
    return to_enum(:each_byte) unless block_given?
    while (b = getbyte)
      yield b
    end
    self
  end

  def each_codepoint
    return to_enum(:each_codepoint) unless block_given?
    each_char { |c| yield c.ord }
    self
  end

  def chars; each_char.to_a; end
  def bytes; each_byte.to_a; end

  # ---- encoding --------------------------------------------------------------
  def set_encoding(ext, int = nil, **opts)
    if ext
      enc = ext.is_a?(Encoding) ? ext : Encoding.find(ext.to_s) rescue nil
      @buf.force_encoding(enc) if enc
    end
    self
  end

  def set_encoding_by_bom
    b = @buf.byteslice(0, 4).to_s
    if b.start_with?("\xEF\xBB\xBF".b)
      @pos = 3 if @pos == 0
      @buf.force_encoding(Encoding::UTF_8)
      Encoding::UTF_8
    else
      nil
    end
  end

  def self.open(string = +"", mode = "r+")
    io = new(string, mode)
    return io unless block_given?
    begin
      yield io
    ensure
      io.close
    end
  end
end
