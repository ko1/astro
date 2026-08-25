class IO
  NULL = '/dev/null'    # the platform's null device
  TimeoutError = Class.new(IOError)   # raised by the timeout-bearing IO ops

  # CRuby: "#<File:/path>", "#<IO:fd 3>", "#<IO:<STDOUT>>", " (closed)" suffix.
  def inspect
    shut = closed?
    name = if defined?(@__io_std_name) && @__io_std_name
             "<#{@__io_std_name}>"
           elsif defined?(@__io_path) && @__io_path
             @__io_path
           elsif shut
             nil            # a closed fd-only IO has no name left to print
           else
             "fd #{fileno}"
           end
    "#<#{self.class}:#{name}#{shut ? "#{name ? ' ' : ''}(closed)" : ''}>"
  end

  # External/internal encodings.  koruby reads produce UTF-8 (ASCII-8BIT in
  # binary mode); these record what was requested so the accessors round-trip
  # and #set_encoding is not a hard error.
  #
  # The pair is modelled exactly as CRuby's rb_io_ext_int_to_encs: @__enc is the
  # stream's encoding and @__enc2 the external one when a transcoding pair was
  # requested.  A nil @__enc means "follow Encoding.default_external", which is
  # what makes an unqualified read stream track later changes to the default.
  # An encoding argument: Encoding / nil pass through, "-" means "not given",
  # anything else converts with #to_str (CRuby).
  private def __enc_arg(v)
    return nil if v.nil?
    return v if v.is_a?(Encoding)
    v = v.to_str if !v.is_a?(String) && v.respond_to?(:to_str)
    # "bom|utf-8" — the BOM prefix only asks that a BOM be honoured on read
    v = v[4..-1] if v.is_a?(String) && v.downcase.start_with?("bom|")
    v == "-" ? nil : v
  end

  # The encoding a read result carries (the C read paths ask once and memoize).
  def __io_read_enc_name
    (internal_encoding || external_encoding || Encoding::UTF_8).name
  end

  def external_encoding
    __resolve_enc
    return @__enc2 if @__enc2
    return @__enc if __io_writable?          # a write stream reports nothing when unset
    @__enc || Encoding.default_external
  end

  def internal_encoding
    __resolve_enc
    return nil unless @__enc2
    @__enc || Encoding.default_external
  end

  # ext/int as requested (nil = not given) and the defaults in effect →
  # the (@__enc, @__enc2) pair.  CRuby's rb_io_ext_int_to_encs, verbatim.
  private def __enc_pair(ext, intern, dext, dint)
    default_ext = ext.nil?
    ext = dext if ext.nil?
    if ext == Encoding::BINARY
      intern = nil                            # ASCII-8BIT external: no transcoding
    elsif intern.nil?
      intern = dint
    end
    if intern.nil? || intern == ext
      @__enc = (default_ext && intern != ext) ? nil : ext
      @__enc2 = nil
    else
      @__enc = intern
      @__enc2 = ext
    end
    __io_enc_reset          # the C read paths memoize the resolved encoding
  end

  # A mode string may carry "…:external[:internal]"; the encodings are resolved
  # once, on first use, against the defaults as they were when the stream opened.
  private def __resolve_enc
    return if @__enc_done
    @__enc_done = true
    ext = nil
    int = nil
    spec = @__io_modestr
    if spec && (i = spec.index(":"))
      e, n = spec[(i + 1)..-1].split(":", 2)
      ext = Encoding.find(e) if e && !e.empty? && e != "-"
      int = Encoding.find(n) if n && !n.empty? && n != "-"
    end
    ext = Encoding::BINARY if ext.nil? && binmode?
    # the defaults as they were when the stream was created (captured in C)
    __enc_pair(ext, int, @__ext_enc0 || Encoding.default_external, @__int_enc0)
  end

  # A leading byte-order mark decides the external encoding (and is consumed).
  # CRuby refuses when the stream is not in binary mode or already carries an
  # encoding of its own.
  def set_encoding_by_bom
    raise ArgumentError, "ASCII incompatible encoding needs binmode" unless binmode?
    __resolve_enc
    raise ArgumentError, "encoding conversion is set" if @__enc2
    if @__enc && @__enc != Encoding::BINARY
      raise ArgumentError, "encoding is set to #{@__enc.name} already"
    end
    name = __io_bom_encoding
    return nil unless name
    set_encoding(name)
    external_encoding
  end

  def set_encoding(*args, **opts)
    if args.empty? || args.length > 2
      raise ArgumentError, "wrong number of arguments (given #{args.length}, expected 1..2)"
    end
    ext, int = args
    raise TypeError, "no implicit conversion of nil into String" if ext.nil? && !int.nil?
    if ext.is_a?(String) && !ext.encoding.ascii_compatible?
      raise ArgumentError, "invalid encoding name (non ASCII)"
    end
    __check_newline_opt(opts[:newline]) if opts.key?(:newline)
    ext = __enc_arg(ext)
    # "-" (or a "ext:-" pair) means "explicitly no internal encoding", which is
    # not the same as leaving it out (that one falls back to the default).
    int_none = (args.length >= 2 && int.is_a?(String) && int == "-")
    int = __enc_arg(int)
    if int.nil? && ext.is_a?(String) && ext.include?(":")
      ext, int = ext.split(":", 2)
      if int == "-" then int = nil; int_none = true end
    end
    ext = Encoding.find(ext) if ext.is_a?(String)
    int = Encoding.find(int) if int.is_a?(String)
    if ext.is_a?(Encoding) && !ext.ascii_compatible? && !binmode? && int.nil? && !__io_writable?
      raise ArgumentError, "ASCII incompatible encoding needs binmode"
    end
    @__enc_done = true          # an explicit set_encoding wins over the mode string
    __enc_pair(ext, int, Encoding.default_external, int_none ? nil : Encoding.default_internal)
    self
  end

  private def __check_newline_opt(v)
    raise ArgumentError, "newline decorator with binary mode" if binmode?
    return if %i[lf crlf cr universal].include?(v)
    raise ArgumentError, "unexpected value for newline option: #{v}" if v.is_a?(Symbol)
    raise ArgumentError, "unexpected value for newline option"
  end


  # IO.new / IO.open / File.open の options Hash のうち、ストリーム生成後に
  # 効くもの (encoding 系・autoclose) を適用する。C 側は :mode / :binmode
  # だけを先に見て、残りをここに渡す。
  # encoding option の値: Encoding / nil はそのまま、他は #to_str で String に。
  private def __enc_opt(v)
    v = v.to_str if !v.nil? && !v.is_a?(Encoding) && !v.is_a?(String) && v.respond_to?(:to_str)
    v == "-" ? nil : v      # "-" は「指定なし」を意味する (CRuby)
  end

  def __apply_open_opts(opts)
    return self unless opts.is_a?(Hash) && !opts.empty?
    @autoclose = opts[:autoclose] ? true : false if opts.key?(:autoclose)
    ext = __enc_opt(opts[:external_encoding])
    int = __enc_opt(opts[:internal_encoding])
    enc = __enc_opt(opts[:encoding])
    if enc && (ext || int)
      # CRuby: 明示的な external/internal が :encoding に勝ち、警告を出す
      warn("Ignoring encoding parameter '#{enc}': #{ext ? 'external' : 'internal'}_encoding is used",
           uplevel: 0)
      enc = nil
    end
    if enc
      if enc.is_a?(String) && enc.include?(":")
        ext, int = enc.split(":", 2)
      else
        ext = enc
      end
    end
    if ext || int
      # the open path is CRuby's rb_io_ext_int_to_encs directly: an internal
      # encoding alone still leaves the external one "not given"
      @__enc_done = true
      __enc_pair(ext.is_a?(String) ? Encoding.find(ext) : ext,
                 int.is_a?(String) ? Encoding.find(int) : int,
                 Encoding.default_external, Encoding.default_internal)
    end
    self
  end
end

class IO
  # IO.open(fd, mode) — like ::new, but a block form closes the stream after.
  # (File overrides this on its own singleton with the path-taking version.)
  def self.open(*args)
    io = new(*args)
    return io unless block_given?
    begin
      yield io
    ensure
      io.close unless io.closed?
    end
  end
end

# FileTest — the File predicates as module functions.
module FileTest
  %i[exist? exists? file? directory? readable? writable? executable? size size?
     zero? symlink? blockdev? chardev? pipe? socket? setgid? setuid? sticky?
     owned? grpowned? world_readable? world_writable? identical? empty?].each do |m|
    next unless File.respond_to?(m)
    define_method(m) { |*a| File.__send__(m, *a) }
    module_function m
  end
end

# Kernel#open — File.open, or IO.popen when the name starts with "|".
module Kernel
  def open(name, *rest, **opts, &blk)
    if name.respond_to?(:to_open)
      io = name.to_open(*rest, **opts)
      return io unless blk
      begin
        return blk.call(io)
      ensure
        io.close if io.respond_to?(:close) && !io.closed?
      end
    end
    path = File.path(name)
    if path.start_with?("|")
      IO.popen(path[1..-1], *rest, **opts, &blk)
    else
      File.open(path, *rest, **opts, &blk)
    end
  end
  module_function :open
end

# IO::WaitReadable / IO::WaitWritable and the Errno::EAGAIN subclasses the
# *_nonblock methods raise, so `rescue IO::WaitReadable` works.
class IO
  module WaitReadable; end
  module WaitWritable; end
  class EAGAINWaitReadable < Errno::EAGAIN; include WaitReadable; end
  class EAGAINWaitWritable < Errno::EAGAIN; include WaitWritable; end
  EWOULDBLOCKWaitReadable = EAGAINWaitReadable          # same errno on Linux
  EWOULDBLOCKWaitWritable = EAGAINWaitWritable
  class EINPROGRESSWaitReadable < Errno::EINPROGRESS; include WaitReadable; end
  class EINPROGRESSWaitWritable < Errno::EINPROGRESS; include WaitWritable; end
end

class IO
  # Byte/codepoint iteration on top of the C getbyte/getc primitives.
  def each_byte(&blk)
    return to_enum(:each_byte) unless blk
    while (b = getbyte)
      blk.call(b)
    end
    self
  end

  def each_codepoint(&blk)
    return to_enum(:each_codepoint) unless blk
    each_char { |ch| blk.call(ch.ord) }
    self
  end
  alias_method :codepoints, :each_codepoint

  def readbyte
    b = getbyte
    raise EOFError, "end of file reached" if b.nil?
    b
  end

  # putc writes a character: a String's first character, or an Integer chr.
  # String なら先頭 1 文字、それ以外は #to_int したバイト値を書く。
  # 空文字列は何も書かない。戻り値は常に引数そのもの。
  def putc(obj)
    if obj.is_a?(String)
      c = obj[0]
      write(c) if c
    elsif obj.nil?
      raise TypeError, "no implicit conversion from nil to integer"
    elsif obj.is_a?(Numeric) || obj.respond_to?(:to_int)
      write((obj.to_int & 0xff).chr)
    else
      raise TypeError, "no implicit conversion of #{obj.class} into Integer"
    end
    obj
  end

  def to_io = self

  # koruby always closes the descriptor with the IO; record the preference.
  def autoclose? = @autoclose.nil? ? true : @autoclose
  def autoclose=(v); @autoclose = v ? true : false; v; end

  def self.try_convert(obj)
    return obj if obj.is_a?(IO)
    return nil unless obj.respond_to?(:to_io)
    io = obj.to_io
    raise TypeError, "can't convert #{obj.class} to IO (#{obj.class}#to_io gives #{io.class})" unless io.is_a?(IO)
    io
  end

  # IO.copy_stream(src, dst[, copy_length[, src_offset]]) — src/dst は path
  # (String/#to_path) でも IO 風オブジェクト (#read / #write) でもよい。path を
  # 渡した側だけこちらで open/close する。src_offset 指定時は src の現在位置を
  # 動かさない (CRuby は pread 相当)。戻り値はコピーしたバイト数。
  def self.copy_stream(src, dst, copy_length = nil, src_offset = nil)
    copy_length = __cs_int(copy_length)
    src_offset = __cs_int(src_offset)
    copy_length = nil if copy_length && copy_length < 0    # 負の length は「全部」
    src_io, src_opened = __cs_io(src, "rb")
    begin
      dst_io, dst_opened = __cs_io(dst, "wb")
      begin
        saved = nil
        if src_offset
          saved = src_io.pos
          src_io.seek(src_offset)
        end
        copied = 0
        chunk = 65536
        # 読めた分だけ即座に書く (#read(n) は n バイト揃うまで待つので、pipe
        # 越しの対話 — 相手が返事を待っている — でデッドロックする)。
        partial = src_io.respond_to?(:readpartial)
        loop do
          want = copy_length ? copy_length - copied : chunk
          want = chunk if want > chunk
          break if want <= 0
          data = nil
          if partial
            begin
              data = src_io.readpartial(want)
            rescue EOFError
              break
            end
          else
            data = src_io.read(want)
          end
          break if data.nil? || data.empty?
          dst_io.write(data)
          dst_io.flush if dst_io.respond_to?(:flush)   # 相手が待っている可能性
          copied += data.bytesize
        end
        src_io.seek(saved) if saved
        # CRuby は dst のディスクリプタへ直接書くので、コピー直後に他から
        # 読めば内容が見える。koruby の write は buffered なので flush する。
        dst_io.flush if dst_io.respond_to?(:flush)
        copied
      ensure
        dst_io.close if dst_opened
      end
    ensure
      src_io.close if src_opened
    end
  end

  # copy_stream の length / offset: nil はそのまま、他は #to_int (CRuby)。
  def self.__cs_int(v)
    return v if v.nil? || v.is_a?(Integer)
    unless v.respond_to?(:to_int)
      raise TypeError, "no implicit conversion of #{v.class} into Integer"
    end
    n = v.to_int
    unless n.is_a?(Integer)
      raise TypeError, "can't convert #{v.class} to Integer (#{v.class}#to_int gives #{n.class})"
    end
    n
  end
  private_class_method :__cs_int

  # → [io, opened_here?].  path (String / #to_path) のときだけ open する。
  def self.__cs_io(obj, mode)
    return [obj, false] if obj.respond_to?(mode == "rb" ? :read : :write) && !obj.is_a?(String)
    path = obj.is_a?(String) ? obj : obj.to_path
    [File.open(path, mode), true]
  end
  private_class_method :__cs_io
end

module Kernel
  # Kernel#putc — $stdout.putc の private ショートカット (CRuby と同じく
  # Kernel の private instance method)。
  private def putc(obj)
    $stdout.putc(obj)
  end
end
