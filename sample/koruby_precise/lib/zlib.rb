# zlib — the Ruby API on top of the C primitives in builtins/zlib.c.
#
# A build without zlib.h leaves those primitives undefined, so requiring this
# file raises LoadError, exactly as a missing extension would.
raise LoadError, "cannot load such file -- zlib" unless respond_to?(:__zlib_version, true)

module Zlib
  ZLIB_VERSION = __zlib_version.freeze
  VERSION      = "3.2.1"

  NO_COMPRESSION      = 0
  BEST_SPEED          = 1
  BEST_COMPRESSION    = 9
  DEFAULT_COMPRESSION = -1

  FILTERED         = 1
  HUFFMAN_ONLY     = 2
  RLE              = 3
  FIXED            = 4
  DEFAULT_STRATEGY = 0

  NO_FLUSH      = 0
  SYNC_FLUSH    = 2
  FULL_FLUSH    = 3
  FINISH        = 4

  BINARY  = 0
  ASCII   = 1
  TEXT    = 1
  UNKNOWN = 2

  MAX_WBITS   = 15
  DEF_MEM_LEVEL = 8
  MAX_MEM_LEVEL = 9
  OS_UNIX = 0x03
  OS_CODE = OS_UNIX

  class Error < StandardError; end
  class StreamEnd < Error; end
  class NeedDict < Error; end
  class DataError < Error; end
  class StreamError < Error; end
  class MemError < Error; end
  class BufError < Error; end
  class VersionError < Error; end
  class InProgressError < Error; end

  module_function

  def zlib_version = ZLIB_VERSION
  # CRuby takes the seed through NUM2ULONG, so anything wider than a machine
  # word is a RangeError rather than a silent truncation.
  def self.__check_seed(args)
    return if args.size < 2
    v = args[1]
    return if v.nil?
    v = v.to_int unless v.is_a?(Integer)
    if v >= (1 << 64) || v < -(1 << 63)
      raise RangeError, "bignum too big to convert into 'unsigned long'"
    end
  end

  def crc32(*args) = (Zlib.__check_seed(args); __zlib_crc32(*args))
  def adler32(*args) = (Zlib.__check_seed(args); __zlib_adler32(*args))
  def crc_table = __zlib_crc_table

  def crc32_combine(crc1, crc2, len2)
    # crc32(a + b) from the two parts: shift crc1 through len2 zero bytes.
    (len2.to_i).times { crc1 = __zlib_crc32("\0".b, crc1) }
    crc1 ^ crc2
  end

  def adler32_combine(a1, a2, len2)
    s1 = a1 & 0xffff
    s2 = (a1 >> 16) & 0xffff
    rem = len2 % 65521
    ((s1 + (a2 & 0xffff) - 1) % 65521) |
      (((rem * s1 + s2 + ((a2 >> 16) & 0xffff) - rem) % 65521) << 16)
  end

  def deflate(str, level = DEFAULT_COMPRESSION) = Deflate.deflate(str, level)
  def inflate(str) = Inflate.inflate(str)

  # With a block, zlib hands the output over in 16 KiB pieces and the call
  # itself answers nil (CRuby's ZSTREAM_AVAIL_OUT_STEP_MAX).
  CHUNK_SIZE = 16384
  def self.__chunked(str, &blk)
    return str unless blk
    i = 0
    while i < str.bytesize
      blk.call(str.byteslice(i, CHUNK_SIZE))
      i += CHUNK_SIZE
    end
    nil
  end

  # A real streaming z_stream lives behind an Integer handle (builtins/zlib.c);
  # #close frees it.  A stream that is never closed leaks its z_stream, which is
  # what a runtime without finalizers can offer.
  class ZStream
    def initialize(deflating, level, window_bits)
      @h = __zstream_open(deflating, level, window_bits)
      @out = +"".b
      @closed = false
    end

    def __stat = (__zstream_stat(@h) || [0, 0, false, 0, 0])
    private :__stat

    def total_in  = __stat[0]
    def total_out = __stat[1] + @out.bytesize
    def stream_end? = __stat[2]
    alias finished? stream_end?
    def avail_in  = __stat[3]
    def avail_out = 0
    def avail_out=(n); n; end
    def data_type = Zlib::UNKNOWN
    def adler = __stat[4]        # zlib's running checksum of the stream
    def closed? = @closed
    alias ended? closed?
    def flush_next_in = +"".b
    def sync(_str) = false

    def close
      raise Zlib::Error, "stream is not ready" if @closed
      __zstream_close(@h)
      @closed = true
      nil
    end
    alias end close

    def flush_next_out
      s = @out
      @out = +"".b
      s
    end

    def reset
      __check_open
      __zstream_close(@h)
      @h = __zstream_open(@deflating, @level || 6, @window_bits)
      @out = +"".b
      nil
    end

    private def __check_open
      raise Zlib::Error, "stream is not ready" if @closed
    end

    # Hand the output over in 16 KiB pieces (ZSTREAM_AVAIL_OUT_STEP_MAX).  A
    # short tail is only yielded once the stream has ended; otherwise it stays
    # buffered for the next call, as in CRuby.  `break' out of the block leaves
    # everything not yet yielded in the buffer.
    private def __emit(str, blk)
      return str unless blk
      n = str.bytesize
      i = 0
      lim = stream_end? ? n : n - (n % CHUNK_SIZE)
      begin
        while i < lim
          j = i
          i += CHUNK_SIZE          # count it before yielding: a `break' consumes it
          blk.call(str.byteslice(j, CHUNK_SIZE))
        end
      ensure
        @out = (i < n ? str.byteslice(i, n - i) : +"".b) + @out
      end
      nil
    end

    # Map the C-side error tag onto the Zlib exception the API documents.
    private def __run(str, flush)
      __zstream_run(@h, str, flush)
    rescue RuntimeError => e
      raise Zlib::DataError, "data error" if e.message.include?("error 1")
      raise Zlib::NeedDict, "need dictionary" if e.message.include?("error 2")
      raise Zlib::BufError, "buffer error"
    end
  end

  class Deflate < ZStream
    def self.deflate(str, level = DEFAULT_COMPRESSION, &blk)
      Zlib.__chunked(__zlib_deflate(str.to_s.b, level == DEFAULT_COMPRESSION ? 6 : level, MAX_WBITS), &blk)
    end

    def initialize(level = DEFAULT_COMPRESSION, window_bits = MAX_WBITS,
                   mem_level = DEF_MEM_LEVEL, strategy = DEFAULT_STRATEGY)
      @deflating = true
      @level = level == DEFAULT_COMPRESSION ? 6 : level
      @window_bits = window_bits
      super(true, @level, window_bits)
    end

    def <<(str)
      __check_open
      @out << __run(str.nil? ? nil : str.to_s.b, NO_FLUSH)
      self
    end

    def deflate(str, flush = NO_FLUSH, &blk)
      __check_open
      @out << __run(str.nil? ? nil : str.to_s.b, str.nil? ? FINISH : flush)
      __emit(flush_next_out, blk)
    end

    def params(level, strategy)
      @level = level == DEFAULT_COMPRESSION ? 6 : level
      nil
    end

    def set_dictionary(dict) = dict

    def finish
      __check_open
      @out << __run(nil, FINISH)
      flush_next_out
    end

    def flush(flush = SYNC_FLUSH)
      __check_open
      @out << __run(nil, flush)
      flush_next_out
    end
  end

  class Inflate < ZStream
    def self.inflate(str, &blk)
      raise Zlib::BufError, "buffer error" if str.nil? || str.empty?
      z = new
      begin
        out = z.inflate(str)
        # a stream that never reached its end was truncated
        raise Zlib::BufError, "buffer error" unless z.stream_end?
        Zlib.__chunked(out + z.finish, &blk)
      ensure
        z.close unless z.closed?
      end
    end

    def initialize(window_bits = MAX_WBITS)
      @deflating = false
      @window_bits = window_bits
      @passthrough = +"".b
      super(false, 0, window_bits)
    end

    def <<(str)
      __check_open
      if str.nil?
        @out << __run(nil, SYNC_FLUSH)
      elsif stream_end?
        @passthrough << str.to_s.b       # after the end, data passes through
      else
        s = str.to_s.b
        @out << __run(s, NO_FLUSH)
        # the stream may have ended mid-argument: the rest passes through
        n = avail_in
        @passthrough << s.byteslice(s.bytesize - n, n) if n > 0
      end
      self
    end

    def inflate(str, buffer = nil, &blk)
      __check_open
      self << str
      return __emit(flush_next_out, blk) if blk
      r = flush_next_out
      buffer ? buffer.replace(r) : r
    end

    def finish(&blk)
      __check_open
      unless stream_end?
        @out << __run(nil, SYNC_FLUSH)
        raise Zlib::BufError, "buffer error" unless stream_end?
      end
      r = flush_next_out + @passthrough
      @passthrough = +"".b
      __emit(r, blk)
    end

    def set_dictionary(dict) = dict
    def sync_point? = false
  end

  # ---- gzip ----------------------------------------------------------------

  class GzipFile
    class Error < Zlib::Error
      attr_reader :input, :inflate
    end
    class CRCError < Error; end
    class LengthError < Error; end
    class NoFooter < Error; end

    OS_CODE = Zlib::OS_CODE

    attr_writer :comment, :orig_name
    attr_accessor :level, :sync
    attr_reader :mtime
    attr_reader :crc, :os_code

    # The header fields live in the stream, so CRuby refuses to read them back
    # once it is closed.
    def comment;   __check_closed; @comment;   end
    def orig_name; __check_closed; @orig_name; end
    private def __check_closed
      raise Zlib::GzipFile::Error, "closed gzip stream" if @closed
    end

    def initialize
      @closed = false
      @os_code = OS_CODE
      @crc = 0
      @sync = false
    end

    def closed? = @closed
    def finish = close
    def to_io = @io
    def path
      raise NoMethodError, "undefined method 'path'" unless @io.respond_to?(:path)
      @io.path
    end

    def self.wrap(*args)
      obj = new(*args)
      return obj unless block_given?
      begin
        yield obj
      ensure
        obj.close unless obj.closed?
      end
    end
  end

  class GzipWriter < GzipFile
    def self.open(filename, level = nil, strategy = nil, **opts, &blk)
      io = File.open(filename, "wb")
      w = new(io, level, strategy)
      return w unless blk
      begin
        blk.call(w)
      ensure
        w.close unless w.closed?
      end
    end

    def initialize(io, level = nil, strategy = nil, **opts)
      super()
      @io = io
      @level = level.nil? || level == DEFAULT_COMPRESSION ? 6 : level
      @buf = +"".b
      @mtime = nil
    end

    # The gzip header carries mtime/orig_name/comment, so they can only be set
    # before anything is written (CRuby writes the header on the first write).
    def mtime=(t)
      __check_header
      @mtime = t.is_a?(Integer) ? Time.at(t) : t
    end
    def orig_name=(n)
      __check_header
      @orig_name = n.to_s
    end
    def comment=(n)
      __check_header
      @comment = n.to_s
    end
    # CRuby emits the header on the first #write, even a zero-length one.
    private def __check_header
      raise Zlib::GzipFile::Error, "header is already written" if @header_written
    end

    def write(*args)
      @header_written = true
      n = 0
      args.each { |a| s = a.to_s.b; @buf << s; n += s.bytesize }
      n
    end
    def <<(s) = (write(s); self)
    def print(*args) = (args.each { |a| write(a) }; nil)
    def printf(fmt, *args) = (write(format(fmt, *args)); nil)
    def putc(ch) = (write(ch.is_a?(Integer) ? (ch & 0xff).chr : ch.to_s[0]); ch)
    def puts(*args)
      return write("\n") && nil if args.empty?
      args.flatten.each { |a| s = a.to_s; write(s); write("\n") unless s.end_with?("\n") }
      nil
    end
    def pos = @buf.bytesize
    alias tell pos
    def flush(v = nil) = self

    def close
      return @io if @closed
      @closed = true
      @crc = Zlib.crc32(@buf)
      @io.write(__gzip_header)
      body = __zlib_deflate(@buf, @level, -MAX_WBITS)      # raw deflate: we write the wrapper
      @io.write(body)
      @io.write([@crc, @buf.bytesize & 0xffffffff].pack("VV"))
      @io.close if @io.respond_to?(:close)
      @io
    end

    private def __gzip_header
      flg = 0
      flg |= 0x08 if @orig_name
      flg |= 0x10 if @comment
      xfl = @level == BEST_COMPRESSION ? 2 : (@level == BEST_SPEED ? 4 : 0)
      h = [0x1f, 0x8b, 8, flg, (@mtime ? @mtime.to_i : 0), xfl, @os_code].pack("CCCCVCC")
      h << @orig_name.b << "\0" if @orig_name
      h << @comment.b << "\0" if @comment
      h
    end
  end

  class GzipReader < GzipFile
    include Enumerable

    def self.open(filename, **opts, &blk)
      io = File.open(filename, "rb")
      r = begin
            new(io, **opts)
          rescue StandardError
            io.close
            raise
          end
      return r unless blk
      begin
        blk.call(r)
      ensure
        r.close unless r.closed?
      end
    end

    def self.zcat(io, **opts, &blk)
      r = new(io, **opts)
      out = r.read
      blk ? blk.call(out) : out
    end

    def initialize(io, **opts)
      super()
      @io = io
      # CRuby always reads with an explicit size, and a source io may only
      # implement that arity.
      raw = +"".b
      while (chunk = io.read(16384))
        break if chunk.empty?
        raw << chunk.b
      end
      @data = __parse(raw)
      @pos = 0
      @pos_bias = 0
      @lineno = 0
      @external_encoding = opts[:external_encoding]
      @internal_encoding = opts[:internal_encoding]
      @encoding = opts[:encoding]
    end

    def read(len = nil, outbuf = nil)
      __check
      if len.nil?
        s = @data.byteslice(@pos..) || +"".b
        @pos = @data.bytesize
        s = s.dup.force_encoding(__enc)
      else
        raise ArgumentError, "negative length #{len} given" if len < 0
        return outbuf ? outbuf.replace(+"".b) : +"".b if len == 0
        return nil if @pos >= @data.bytesize
        s = @data.byteslice(@pos, len) || +"".b
        @pos += s.bytesize
      end
      outbuf ? outbuf.replace(s) : s
    end

    def readpartial(len, outbuf = nil)
      raise EOFError, "end of file reached" if @pos >= @data.bytesize && len > 0
      read(len, outbuf)
    end

    def gets(sep = $/, limit = nil)
      __check
      if sep == ""                      # paragraph mode: skip blank lines, stop at "\n\n"
        @pos += 1 while @pos < @data.bytesize && @data.getbyte(@pos) == 10
        return nil if @pos >= @data.bytesize
        rest = @data.byteslice(@pos..)
        i = rest.index("\n\n".b)
        line = i ? rest.byteslice(0, i + 2) : rest
        @pos += line.bytesize
        # the run of newlines after the paragraph is consumed but not returned
        @pos += 1 while @pos < @data.bytesize && @data.getbyte(@pos) == 10
        @lineno += 1
        return ($_ = line.dup.force_encoding(__enc))
      end
      return nil if @pos >= @data.bytesize
      rest = @data.byteslice(@pos..)
      i = sep ? rest.index(sep) : nil
      line = i ? rest[0, i + sep.length] : rest
      line = line[0, limit] if limit && line.length > limit
      @pos += line.bytesize
      @lineno += 1
      $_ = line.dup.force_encoding(__enc)
    end

    def readline(sep = $/) = (gets(sep) or raise EOFError, "end of file reached")
    def each(sep = $/, &blk)
      return to_enum(:each, sep) unless blk
      while (l = gets(sep)); blk.call(l); end
      self
    end
    alias each_line each
    def readlines(sep = $/) = (a = []; each(sep) { |l| a << l }; a)
    def lines(sep = $/) = readlines(sep)

    def getc
      __check
      return nil if @pos >= @data.bytesize
      c = @data.byteslice(@pos, 4).to_s.force_encoding(__enc)[0]
      @pos += c.bytesize
      c
    end
    def readchar = (getc or raise EOFError, "end of file reached")
    def getbyte
      return nil if @pos >= @data.bytesize
      b = @data.getbyte(@pos); @pos += 1; b
    end
    def readbyte = (getbyte or raise EOFError, "end of file reached")
    def each_char(&blk) = (return to_enum(:each_char) unless blk; while (c = getc); blk.call(c); end; self)
    def each_byte(&blk) = (return to_enum(:each_byte) unless blk; while (b = getbyte); blk.call(b); end; self)
    # CRuby splices the character back into the stream (it need not be the one
    # that was read), so rebuild the buffer rather than just moving the cursor.
    def ungetc(c)
      s = c.is_a?(Integer) ? c.chr(__enc) : c.to_s
      __unget(s.b)
    end

    def ungetbyte(b)
      s = b.is_a?(Integer) ? (b & 0xff).chr : b.to_s
      __unget(s.b)
    end

    # #pos goes NEGATIVE when more is pushed back than was read (CRuby counts
    # the pushback against the original stream position), so track the shift.
    private def __unget(bytes)
      @data = @data.byteslice(0, @pos).to_s + bytes + @data.byteslice(@pos..).to_s
      @pos_bias -= bytes.bytesize
      nil
    end

    def eof? = @pos >= @data.bytesize
    alias eof eof?
    def pos = @pos + @pos_bias
    alias tell pos
    def lineno = @lineno
    def lineno=(n); @lineno = n; end
    def rewind
      @io.seek(0, IO::SEEK_SET) if @io.respond_to?(:seek)   # CRuby rewinds the source io
      @pos = 0
      @pos_bias = 0
      @lineno = 0
      0
    end
    def unused = nil
    def external_encoding = @encoding || @external_encoding || Encoding.default_external

    def close
      @closed = true
      @io.close if @io.respond_to?(:close)
      @io
    end

    private def __enc = @encoding || @external_encoding || Encoding::BINARY
    private def __check
      raise IOError, "closed gzip stream" if @closed
    end

    # Parse the gzip wrapper, inflate the body, and verify CRC + ISIZE.
    private def __parse(raw)
      raise Zlib::GzipFile::Error, "not in gzip format" if raw.bytesize < 18
      raise Zlib::GzipFile::Error, "not in gzip format" unless raw.getbyte(0) == 0x1f && raw.getbyte(1) == 0x8b
      raise Zlib::GzipFile::Error, "unsupported compression method" unless raw.getbyte(2) == 8
      flg = raw.getbyte(3)
      @mtime = Time.at(raw.byteslice(4, 4).unpack1("V"))
      @level = raw.getbyte(8) == 2 ? BEST_COMPRESSION : (raw.getbyte(8) == 4 ? BEST_SPEED : DEFAULT_COMPRESSION)
      @os_code = raw.getbyte(9)
      i = 10
      if (flg & 0x04) != 0                                  # FEXTRA
        xlen = raw.byteslice(i, 2).unpack1("v"); i += 2 + xlen
      end
      if (flg & 0x08) != 0                                  # FNAME
        j = raw.index("\0".b, i); @orig_name = raw.byteslice(i, j - i); i = j + 1
      end
      if (flg & 0x10) != 0                                  # FCOMMENT
        j = raw.index("\0".b, i); @comment = raw.byteslice(i, j - i); i = j + 1
      end
      i += 2 if (flg & 0x02) != 0                           # FHCRC
      body = raw.byteslice(i, raw.bytesize - i - 8)
      raise Zlib::GzipFile::NoFooter, "footer is not found" if body.nil?
      out = begin
              __zlib_inflate(body, -MAX_WBITS)
            rescue RuntimeError
              raise Zlib::GzipFile::Error, "invalid compressed data"
            end
      want_crc, want_len = raw.byteslice(raw.bytesize - 8, 8).unpack("VV")
      @crc = Zlib.crc32(out)
      raise Zlib::GzipFile::LengthError, "invalid compressed data -- length error" if want_len != (out.bytesize & 0xffffffff)
      raise Zlib::GzipFile::CRCError, "invalid compressed data -- crc error" if want_crc != @crc
      out
    end
  end

  def gzip(str, level: nil, strategy: nil)
    io = StringIO.new(+"".b)
    w = GzipWriter.new(io, level, strategy)
    w.write(str)
    w.close
    io.string
  end

  def gunzip(str)
    GzipReader.new(StringIO.new(str.b)).read
  end
  module_function :gzip, :gunzip
end
