class IO
  # External/internal encodings.  koruby reads produce UTF-8 (ASCII-8BIT in
  # binary mode); these record what was requested so the accessors round-trip
  # and #set_encoding is not a hard error.
  def external_encoding
    __resolve_enc
    return @__ext_enc if @__ext_enc
    return Encoding::BINARY if binmode?
    Encoding.default_external
  end

  def internal_encoding
    __resolve_enc
    @__int_enc
  end

  # A mode string may carry "…:external[:internal]"; if it does not, the
  # Encoding.default_internal captured when the stream was opened applies.
  private def __resolve_enc
    return if @__enc_done
    @__enc_done = true
    spec = @__io_modestr
    if spec && (i = spec.index(":"))
      ext, int = spec[(i + 1)..-1].split(":", 2)
      @__ext_enc ||= Encoding.find(ext) if ext && !ext.empty?
      @__int_enc ||= Encoding.find(int) if int && !int.empty?
    end
    @__int_enc ||= @__int_enc0
    @__int_enc = nil if @__int_enc && @__int_enc == (@__ext_enc || Encoding.default_external)
  end

  def set_encoding(ext = nil, int = nil, **opts)
    if int.nil? && ext.is_a?(String) && ext.include?(":")
      ext, int = ext.split(":", 2)
    end
    @__ext_enc = ext.nil? ? nil : (ext.is_a?(Encoding) ? ext : Encoding.find(ext))
    @__int_enc = int.nil? ? nil : (int.is_a?(Encoding) ? int : Encoding.find(int))
    @__int_enc = nil if @__int_enc && @__int_enc == @__ext_enc
    @__enc_done = true          # an explicit set_encoding wins over the mode string
    self
  end

  def set_encoding_by_bom
    nil
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
  def putc(obj)
    if obj.is_a?(String)
      write(obj[0])
    elsif obj.is_a?(Numeric)
      write((obj.to_int & 0xff).chr)
    elsif obj.respond_to?(:to_str)
      write(obj.to_str[0])
    else
      raise TypeError, "no implicit conversion of #{obj.class} into String"
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
end
