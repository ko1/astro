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
  def self.default_external; UTF_8; end
  def self.default_internal; nil; end
end
class Encoding
  class Converter; end
  class CompatibilityError < StandardError; end
  class UndefinedConversionError < StandardError; end
  class InvalidByteSequenceError < StandardError; end
  class ConverterNotFoundError < StandardError; end
end
class String
  def encoding; Encoding::UTF_8; end
end
