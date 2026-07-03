# Minimal Encoding: enough for constant references and identity comparison so
# specs that mention Encoding::X in setup don't crash.  Per-string encoding
# tracking is out of scope (String#encoding returns UTF-8, the default).
class Encoding
  def initialize(name); @name = name; end
  def name; @name; end
  def to_s; @name; end
  def inspect; "#<Encoding:" + @name + ">"; end
  def ==(o); o.is_a?(Encoding) && o.name == @name; end
  def ascii_compatible?; @name != "UTF-16" && @name != "UTF-32"; end
  def dummy?; false; end
  UTF_8 = Encoding.new("UTF-8")
  US_ASCII = Encoding.new("US-ASCII")
  ASCII = US_ASCII
  ASCII_8BIT = Encoding.new("ASCII-8BIT")
  BINARY = ASCII_8BIT
  UTF_16 = Encoding.new("UTF-16")
  UTF_32 = Encoding.new("UTF-32")
  UTF_16LE = Encoding.new("UTF-16LE")
  UTF_32LE = Encoding.new("UTF-32LE")
  UTF_7 = Encoding.new("UTF-7")
  CESU_8 = Encoding.new("CESU-8")
  SHIFT_JIS = Encoding.new("Shift_JIS")
  EUC_JP = Encoding.new("EUC-JP")
  ISO_2022_JP = Encoding.new("ISO-2022-JP")
  CP50221 = Encoding.new("CP50221")
  ISO_8859_1 = Encoding.new("ISO-8859-1")
  Windows_1252 = Encoding.new("Windows-1252")
  @@default_external = UTF_8
  @@default_internal = nil
  def self.default_external; @@default_external; end
  def self.default_external=(e); @@default_external = e; end
  def self.default_internal; @@default_internal; end
  def self.default_internal=(e); @@default_internal = e; end
end
class Encoding
  class Converter; end
  class CompatibilityError < StandardError; end
  class UndefinedConversionError < StandardError; end
  class InvalidByteSequenceError < StandardError; end
  class ConverterNotFoundError < StandardError; end
end
class String
  # encoding is tracked as a small tag in the string header (0 UTF-8, 1 US-ASCII,
  # 2 ASCII-8BIT); the C primitives read/set it.  Only these three encodings are
  # distinguished — enough for #encoding / #force_encoding / #b and the ASCII /
  # binary specs.  (Full per-encoding semantics / negotiation are out of scope.)
  def encoding
    case __encoding_tag
    when 1 then Encoding::US_ASCII
    when 2 then Encoding::ASCII_8BIT
    else Encoding::UTF_8
    end
  end
  def force_encoding(enc)
    name = enc.is_a?(Encoding) ? enc.name : enc.to_str
    __set_encoding_tag(case name
                       when "ASCII-8BIT", "BINARY" then 2
                       when "US-ASCII", "ASCII", "ANSI_X3.4-1968" then 1
                       else 0
                       end)
    self
  end
  def b
    dup.force_encoding("ASCII-8BIT")
  end
end
