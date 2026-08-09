# StringIO — an in-memory IO-alike backed by a String buffer + read position.
# Pure Ruby (the real stdlib StringIO is C); covers the common read/write API.
class StringIO
  include Enumerable

  def initialize(string = +"", mode = "r+")
    @buf = string.dup
    @pos = 0
  end

  def string; @buf; end
  def to_s;   @buf; end
  def size;   @buf.length; end
  def length; @buf.length; end
  def pos;    @pos; end
  def pos=(n); @pos = n; end
  def rewind;  @pos = 0; end
  def eof?;    @pos >= @buf.length; end
  alias eof eof?
  def close; nil; end
  def closed?; false; end
  def flush; self; end
  def sync; true; end
  def sync=(v); v; end
  def fileno; nil; end

  def write(*args)
    n = 0
    args.each { |a| s = a.to_s; @buf << s; n += s.length }
    n
  end
  def internal_encoding; nil; end
  def external_encoding; @buf.encoding; end
  def binmode; self; end
  def seek(amount, whence = 0)
    case whence
    when 1 then @pos += amount
    when 2 then @pos = @buf.bytesize + amount
    else        @pos = amount
    end
    @pos = 0 if @pos < 0
    0
  end

  def <<(obj)
    @buf << obj.to_s
    self
  end

  def print(*args)
    args.each { |a| @buf << a.to_s }
    nil
  end

  def printf(fmt, *args)
    @buf << format(fmt, *args)
    nil
  end

  def puts(*args)
    if args.empty?
      @buf << "\n"
    else
      args.each do |a|
        if a.is_a?(Array)
          puts(*a)
        else
          s = a.to_s
          @buf << s
          @buf << "\n" unless s.end_with?("\n")
        end
      end
    end
    nil
  end

  def read(length = nil, outbuf = nil)
    return nil if length && @pos >= @buf.length
    rest = @buf[@pos..] || ""
    r = length ? rest[0, length] : rest
    @pos += r.length
    r
  end

  def getc
    return nil if @pos >= @buf.length
    ch = @buf[@pos]
    @pos += 1
    ch
  end

  def gets(sep = "\n")
    return nil if @pos >= @buf.length
    idx = @buf.index(sep, @pos)
    if idx
      line = @buf[@pos..idx + sep.length - 1]
      @pos = idx + sep.length
    else
      line = @buf[@pos..]
      @pos = @buf.length
    end
    line
  end

  def readline(sep = "\n")
    line = gets(sep)
    raise EOFError, "end of file reached" if line.nil?
    line
  end

  def each_line(sep = "\n")
    return enum_for(:each_line, sep) unless block_given?
    while (l = gets(sep))
      yield l
    end
    self
  end
  alias each each_line

  def readlines(sep = "\n")
    lines = []
    while (l = gets(sep))
      lines << l
    end
    lines
  end

  def each_char
    return enum_for(:each_char) unless block_given?
    while (c = getc)
      yield c
    end
    self
  end

  def getbyte
    boff = @buf[0...@pos].bytesize
    return nil if boff >= @buf.bytesize
    b = @buf.getbyte(boff)
    @pos += 1                                 # advance one character (ASCII: one byte)
    b
  end
  def each_byte
    return enum_for(:each_byte) unless block_given?
    boff = @buf[0...@pos].bytesize
    all = @buf.bytes
    while boff < all.length
      yield all[boff]
      boff += 1
    end
    @pos = @buf.length
    self
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
