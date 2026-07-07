# Minimal Encoding: enough for constant references and identity comparison so
# specs that mention Encoding::X in setup don't crash.  Per-string encoding
# tracking is out of scope (String#encoding returns UTF-8, the default).
class Encoding
  def initialize(name); @name = name; end
  def name; @name; end
  def to_s; @name; end
  def inspect; @name == "ASCII-8BIT" ? "#<Encoding:BINARY (ASCII-8BIT)>" : "#<Encoding:" + @name + ">"; end
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
  Shift_JIS = SHIFT_JIS                    # CRuby's actual constant name (mixed case)
  Windows_31J = Encoding.new("Windows-31J")
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
  # Encoding.find(name) → the Encoding constant whose #name matches (case-folded),
  # else a fresh Encoding of that name (used by String#encoding for "other").
  def self.find(name)
    n = name.to_s
    constants.each do |cn|
      e = const_get(cn)
      return e if e.is_a?(Encoding) && (e.name == n || e.name.upcase == n.upcase)
    end
    Encoding.new(n)
  end
  # Alias → canonical encoding name.
  def self.aliases
    { 'BINARY' => 'ASCII-8BIT', 'ASCII' => 'US-ASCII', 'ANSI_X3.4-1968' => 'US-ASCII',
      'UTF8' => 'UTF-8', 'SJIS' => 'Shift_JIS', 'CP932' => 'Windows-31J', 'eucJP' => 'EUC-JP',
      'external' => default_external.name, 'locale' => default_external.name }
  end
  def self.list
    r = []; constants.each { |cn| e = const_get(cn); r << e if e.is_a?(Encoding) && !r.include?(e) }; r
  end
  def self.name_list; list.map(&:name) + aliases.keys; end
  def self.locale_charmap; default_external.name; end
  # This encoding's canonical name plus any aliases pointing to it.
  def names
    r = [@name]
    Encoding.aliases.each { |a, canon| r << a if canon == @name && a != 'external' && a != 'locale' }
    r
  end
  # Encoding.compatible?(obj1, obj2) → the encoding two objects can share, else nil.
  # Follows CRuby's rb_enc_compatible for ASCII-compatible encodings: same encoding
  # wins; otherwise an ASCII-only operand yields to the other's encoding.
  def self.compatible?(a, b)
    ea = a.is_a?(Encoding) ? a : (a.respond_to?(:encoding) ? a.encoding : nil)
    eb = b.is_a?(Encoding) ? b : (b.respond_to?(:encoding) ? b.encoding : nil)
    return nil if ea.nil? || eb.nil?
    return ea if ea == eb
    # "ASCII-only" coderange: a String/Symbol exposes it directly; a bare Encoding
    # has no content, so only US-ASCII (which can hold nothing but ASCII) counts.
    a1 = a.is_a?(String) ? a.ascii_only? : (a.is_a?(Symbol) ? a.to_s.ascii_only? : ea == US_ASCII)
    b1 = b.is_a?(String) ? b.ascii_only? : (b.is_a?(Symbol) ? b.to_s.ascii_only? : eb == US_ASCII)
    return nil unless ea.ascii_compatible? && eb.ascii_compatible?
    if a1 && b1 then ea
    elsif a1 then eb
    elsif b1 then ea
    else nil
    end
  end
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
    when 0 then Encoding::UTF_8
    when 1 then Encoding::US_ASCII
    when 2 then Encoding::ASCII_8BIT
    else Encoding.find(__encoding_name)   # "other" (index 3+): by stored name
    end
  end
  # force_encoding / b are C methods (builtins/string.c).
end
class Symbol
  # A Symbol reports US-ASCII when its name is ASCII-only, else UTF-8 (CRuby).
  def encoding
    to_s.ascii_only? ? Encoding::US_ASCII : Encoding::UTF_8
  end
end
