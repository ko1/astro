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
