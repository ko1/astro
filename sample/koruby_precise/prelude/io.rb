class IO
  # External/internal encodings.  koruby reads produce UTF-8 (ASCII-8BIT in
  # binary mode); these record what was requested so the accessors round-trip
  # and #set_encoding is not a hard error.
  def external_encoding
    e = @__ext_enc
    return e if e
    return Encoding::BINARY if binmode?
    Encoding.default_external
  end

  def internal_encoding = @__int_enc

  def set_encoding(ext = nil, int = nil, **opts)
    if int.nil? && ext.is_a?(String) && ext.include?(":")
      ext, int = ext.split(":", 2)
    end
    @__ext_enc = ext.nil? ? nil : (ext.is_a?(Encoding) ? ext : Encoding.find(ext))
    @__int_enc = int.nil? ? nil : (int.is_a?(Encoding) ? int : Encoding.find(int))
    @__int_enc = nil if @__int_enc && @__int_enc == @__ext_enc
    self
  end

  def set_encoding_by_bom
    nil
  end
end
