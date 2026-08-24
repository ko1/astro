# StringIO — an in-memory IO-alike backed by a String buffer + read position.
# Pure Ruby (the real stdlib StringIO is C); covers the read/write API incl.
# mode tracking (closed_read?/closed_write?), pos-aware writes, ungetc,
# sysread/readpartial/nonblock variants and lineno.
class StringIO
  VERSION = "3.2.0" unless const_defined?(:VERSION)
  include Enumerable

  # Integer modes (File::RDONLY etc.), same bits CRuby uses.
  MODE_RDONLY__ = 0
  MODE_WRONLY__ = 1
  MODE_RDWR__   = 2
  MODE_APPEND__ = 1024
  MODE_TRUNC__  = 512

  def initialize(*args, **opts)
    no_string = args.empty?
    string = no_string ? +"" : args[0]
    mode = args.length > 1 ? args[1] : nil
    unless string.is_a?(String)
      unless string.respond_to?(:to_str)
        raise TypeError, "no implicit conversion of #{string.class} into String"
      end
      string = string.to_str
    end
    @buf = string
    mode = opts[:mode] if mode.nil? && opts.key?(:mode)   # StringIO.new(str, mode: "r")
    truncate_int = false
    if mode.is_a?(Integer)                                # File::RDONLY / WRONLY | TRUNC / ...
      acc = mode & 3
      m = acc == MODE_WRONLY__ ? "w" : acc == MODE_RDWR__ ? "r+" : "r"
      m = "w" if (mode & MODE_TRUNC__) != 0 && acc != MODE_RDONLY__
      m = "a" if (mode & MODE_APPEND__) != 0 && acc != MODE_RDONLY__
      m += "+" if acc == MODE_RDWR__ && !m.include?("+")
      truncate_int = (mode & MODE_TRUNC__) != 0
      mode = m
      @int_mode__ = true      # an Integer mode never truncates on its own
    elsif !mode.nil? && !mode.is_a?(String)
      unless mode.respond_to?(:to_str)
        raise TypeError, "no implicit conversion of #{mode.class} into String"
      end
      mode = mode.to_str
    end
    mode ||= string.frozen? ? "r" : "r+"
    mode = mode.to_s
    # CRuby refuses a spelling given both in the mode string and as a keyword
    if mode.include?(":") && (opts.key?(:encoding) || opts.key?(:external_encoding) ||
                              opts.key?(:internal_encoding))
      raise ArgumentError, "encoding specified twice"
    end
    if (mode.include?("b") || mode.include?("t")) &&
       (opts.key?(:binmode) || opts.key?(:textmode))
      raise ArgumentError, "binmode specified twice"
    end
    if opts[:binmode] && opts[:textmode]
      raise ArgumentError, "both textmode and binmode specified"
    end
    enc_spec = nil
    if (colon = mode.index(":"))
      enc_spec = mode[(colon + 1)..]
      mode = mode[0, colon]
    end
    enc_spec ||= opts[:encoding] || opts[:external_encoding]
    mode += "b" if opts[:binmode] && !mode.include?("b")
    @append = mode.start_with?("a")
    plus = mode.include?("+")
    @readable = plus || mode.start_with?("r")
    @writable = plus || mode.start_with?("w") || @append
    if @buf.frozen? && @writable
      # CRuby refuses a frozen backing string for a writable StringIO
      raise Errno::EACCES
    end
    if @buf.frozen? && truncate_int
      raise FrozenError, "can't modify frozen String: #{@buf.inspect}"
    end
    @buf.clear if (mode.start_with?("w") && !@int_mode__) || truncate_int
    if enc_spec                                     # "w:ISO-8859-1" / encoding: keyword
      @buf.force_encoding(enc_spec.split(":").first)
    elsif mode.include?("b")
      @buf.force_encoding(Encoding::BINARY)
    elsif no_string
      @buf.force_encoding(Encoding.default_external)   # only the implicit "" buffer
    end
    @pos = 0
    @lineno = 0
    @closed_read = false
    @closed_write = false
  end

  # CRuby's StringIO#inspect is the bare Object#to_s form (never the buffer).
  def inspect = to_s

  def string; @buf; end
  def string=(s); @buf = s; @pos = 0; @lineno = 0; s; end
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

  def reopen(*args)
    other = args.empty? ? +"" : args[0]
    mode = args.length > 1 ? args[1] : nil
    # one non-String argument is converted with #to_strio, never #to_str (CRuby)
    if !other.is_a?(StringIO) && !other.is_a?(String) && mode.nil?
      raise TypeError, "no implicit conversion of #{other.class} into String" unless other.respond_to?(:to_strio)
      other = other.to_strio
      unless other.is_a?(StringIO)
        raise TypeError, "can't convert to StringIO (#to_strio gives #{other.class})"
      end
    end
    if other.is_a?(StringIO)
      @buf = other.string
      @pos = other.pos
      @readable = !other.closed_read?
      @writable = !other.closed_write?
      @append = false
    else
      unless other.is_a?(String)
        unless other.respond_to?(:to_str)
          raise TypeError, "no implicit conversion of #{other.class} into String"
        end
        other = other.to_str
      end
      initialize(other, mode || (other.frozen? ? "r" : "r+"))
    end
    @closed_read = false
    @closed_write = false
    @lineno = 0
    self
  end

  # #to_int / #to_str coercion, exactly once per call (CRuby's Check_Type path).
  def __to_int(v, what = "Integer")
    return v if v.is_a?(Integer)
    raise TypeError, "no implicit conversion of #{v.class} into #{what}" unless v.respond_to?(:to_int)
    r = v.to_int
    raise TypeError, "can't convert #{v.class} to Integer (#{v.class}#to_int gives #{r.class})" unless r.is_a?(Integer)
    r
  end
  def __to_str(v)
    return v if v.is_a?(String)
    raise TypeError, "no implicit conversion of #{v.class} into String" unless v.respond_to?(:to_str)
    r = v.to_str
    raise TypeError, "can't convert #{v.class} to String (#{v.class}#to_str gives #{r.class})" unless r.is_a?(String)
    r
  end
  private :__to_int, :__to_str

  def __check_readable
    raise IOError, "not opened for reading" if closed_read?
  end
  def __check_writable
    raise IOError, "not opened for writing" if closed_write?
  end
  private :__check_readable, :__check_writable

  def seek(amount, whence = 0)
    amount = __to_int(amount)
    whence = __to_int(whence)
    raise Errno::EINVAL, "Invalid argument" unless (0..2).cover?(whence)
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

  # IO#putc と同じ規則: String は先頭 1 文字、他は #to_int したバイト値。
  def putc(ch)
    __check_writable
    if ch.is_a?(String)
      c = ch[0]
      write(c) if c
    elsif ch.nil?
      raise TypeError, "no implicit conversion from nil to integer"
    elsif ch.is_a?(Numeric) || ch.respond_to?(:to_int)
      write((ch.to_int & 0xff).chr)
    else
      raise TypeError, "no implicit conversion of #{ch.class} into Integer"
    end
    ch
  end

  def puts(*args)
    return write("\n") && nil if args.empty?
    __puts_each(args, [])
    nil
  end

  # `seen` carries the Arrays currently being printed so a self-recursive one
  # renders as "[...]" instead of recursing forever (CRuby).
  private def __puts_each(args, seen)
    args.each do |a|
      ary = a.is_a?(Array) ? a : (a.respond_to?(:to_ary) ? a.to_ary : nil)
      if ary
        if seen.any? { |s| s.equal?(a) }
          write("[...]\n")
        else
          seen.push(a)
          begin
            ary.empty? ? write("\n") : __puts_each(ary, seen)
          ensure
            seen.pop
          end
        end
        next
      end
      s = a.nil? ? "" : a
      unless s.is_a?(String)
        t = s.to_s
        s = t.is_a?(String) ? t : __obj_info(a)     # a #to_s that isn't a String
      end
      write(s)
      write("\n") unless s.end_with?("\n")
    end
  end

  # CRuby falls back to the address-only form of #inspect (no ivars).
  private def __obj_info(o) = o.inspect.split(" ")[0] + ">"

  def truncate(n)
    __check_writable
    n = __to_int(n)
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
    length = __to_int(length) unless length.nil?
    outbuf = __to_str(outbuf) unless outbuf.nil?
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
    length = __to_int(length) unless length.nil?
    outbuf = __to_str(outbuf) unless outbuf.nil?
    __check_readable
    return (outbuf ? outbuf.replace("") : "") if length.nil? && eof?   # a full read at EOF is ""
    raise EOFError, "end of file reached" if eof?
    r = read(length, outbuf)
    r = r.dup.force_encoding(Encoding::BINARY) if r && length   # a sized read is binary (CRuby)
    outbuf ? outbuf.replace(r) : r
  end
  # io/console's StringIO extension: one character, ignoring the tty options.
  def getch(*) = getc
  def getpass(*) = gets
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
    return nil if ch.nil?
    s = ch.is_a?(Integer) ? ch.chr : __to_str(ch)
    if @pos > @buf.bytesize                         # past the end: pad the gap with NUL
      @buf << ("\0" * (@pos - @buf.bytesize))
    end
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

  # Coerce the (sep, limit) pair once — #each_line must not re-run a #to_str /
  # #to_int conversion per line.
  private def __line_args(sep, limit)
    if !sep.nil? && !sep.is_a?(String) && limit.nil?
      # one non-String argument: a separator only if it is String-like,
      # otherwise it is the limit (CRuby's rb_io_getline_prepare)
      if sep.respond_to?(:to_str)
        sep = sep.to_str
      else
        limit = sep
        sep = $/
      end
    elsif !sep.nil? && !sep.is_a?(String)
      raise TypeError, "no implicit conversion of #{sep.class} into String" unless sep.respond_to?(:to_str)
      sep = sep.to_str
    end
    if !limit.nil? && !limit.is_a?(Integer)
      raise TypeError, "no implicit conversion of #{limit.class} into Integer" unless limit.respond_to?(:to_int)
      limit = limit.to_int
    end
    limit = nil if limit && limit < 0               # a negative limit means "no limit"
    [sep, limit]
  end

  # One line with already-coerced arguments; does NOT touch $_ (#each_line and
  # friends leave it alone, only #gets/#readline set it).
  private def __getline(sep, limit, chomp)
    __check_readable
    return nil if eof?
    if sep.nil?
      line = read(limit)
    elsif sep == ""                                 # paragraph mode
      rest = @buf.byteslice(@pos, @buf.bytesize - @pos)
      lead = rest[/\A\n*/].length                    # leading blank lines are skipped
      body = rest[lead..]
      i = body.index("\n\n")
      if i
        stop = i + 2
        stop += 1 while body[stop] == "\n"           # the paragraph keeps every separator newline
        line = body[0, stop]
      else
        line = body
      end
      line = line[0, limit] if limit && line.length > limit
      @pos += lead + line.bytesize
    else
      rest = @buf.byteslice(@pos, @buf.bytesize - @pos)
      i = rest.index(sep)
      line = i ? rest[0, i + sep.length] : rest
      line = line[0, limit] if limit && line.length > limit
      @pos += line.bytesize
    end
    @lineno += 1
    return line unless chomp
    sep.nil? || sep == "\n" ? line.chomp : line.delete_suffix(sep)
  end

  def gets(sep = $/, limit = nil, chomp: false)
    sep, limit = __line_args(sep, limit)
    $_ = __getline(sep, limit, chomp)               # CRuby: #gets sets $_
  end

  def readline(sep = $/, limit = nil, chomp: false)
    raise EOFError, "end of file reached" if eof?
    gets(sep, limit, chomp: chomp)
  end

  def each_line(sep = $/, limit = nil, chomp: false)
    return to_enum(:each_line, sep, limit, chomp: chomp) unless block_given?
    sep, limit = __line_args(sep, limit)
    while (l = __getline(sep, limit, chomp))
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

  # The BOMs CRuby recognises, longest first so UTF-32LE wins over UTF-16LE.
  BOMS__ = [
    ["\x00\x00\xFE\xFF".b, Encoding::UTF_32BE],
    ["\xFF\xFE\x00\x00".b, Encoding::UTF_32LE],
    ["\xEF\xBB\xBF".b,      Encoding::UTF_8],
    ["\xFE\xFF".b,          Encoding::UTF_16BE],
    ["\xFF\xFE".b,          Encoding::UTF_16LE],
  ].freeze

  def set_encoding_by_bom
    head = @buf.byteslice(0, 4).to_s.b
    BOMS__.each do |bom, enc|
      next unless head.start_with?(bom)
      @pos = bom.bytesize if @pos == 0
      @buf.force_encoding(enc)
      return enc
    end
    nil
  end

  def self.open(*args, **opts)
    io = new(*args, **opts)
    return io unless block_given?
    begin
      yield io
    ensure
      io.close
      io.__drop_string            # CRuby drops the buffer when the block ends
    end
  end

  def __drop_string
    @buf = nil
    nil
  end
end
