# Minimal Encoding: enough for constant references and identity comparison so
# specs that mention Encoding::X in setup don't crash.  Per-string encoding
# tracking is out of scope (String#encoding returns UTF-8, the default).
class Encoding
  def initialize(name); @name = name; end
  def name; @name; end
  def to_s; @name; end
  def inspect; @name == "ASCII-8BIT" ? "#<Encoding:BINARY (ASCII-8BIT)>" : "#<Encoding:" + @name + ">"; end
  def ==(o); o.is_a?(Encoding) && o.name == @name; end
  # ASCII-compatible = one byte per ASCII char, mapping to itself.  The UTF-16 /
  # UTF-32 families (including the explicit-endian variants), UTF-7 and the
  # stateful ISO-2022 / CP502xx encodings are not.
  def ascii_compatible?
    n = @name.upcase
    !(n.start_with?("UTF-16") || n.start_with?("UTF-32") || n == "UTF-7" ||
      n.start_with?("ISO-2022") || n.start_with?("CP502"))
  end
  def dummy?
    n = @name.upcase
    n == "UTF-16" || n == "UTF-32" || n == "UTF-7" || n.start_with?("ISO-2022") || n.start_with?("CP502")
  end
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
  KOI8_U = Encoding.new("KOI8-U"); KOI8_R = Encoding.new("KOI8-R")
  UTF_16BE = Encoding.new("UTF-16BE"); UTF_16LE = Encoding.new("UTF-16LE")
  UTF_32BE = Encoding.new("UTF-32BE"); UTF_32LE = Encoding.new("UTF-32LE")
  EUC_KR = Encoding.new("EUC-KR"); GB18030 = Encoding.new("GB18030"); Big5 = Encoding.new("Big5")
  Emacs_Mule = Encoding.new("Emacs-Mule"); EmacsMule = Emacs_Mule
  ISO_8859_2 = Encoding.new("ISO-8859-2"); ISO_8859_15 = Encoding.new("ISO-8859-15")
  # Every Encoding constant CRuby defines.  koruby models UTF-8 / US-ASCII /
  # ASCII-8BIT for real; the rest exist so specs that merely name them (and
  # #name / #ascii_compatible? / #dummy? on them) resolve.
  ASCII_8BIT = Encoding.new("ASCII-8BIT") unless const_defined?(:ASCII_8BIT, false)
  BINARY = ASCII_8BIT unless const_defined?(:BINARY, false)
  BIG5 = Encoding.new("Big5") unless const_defined?(:BIG5, false)
  Big5 = BIG5 unless const_defined?(:Big5, false)
  BIG5_HKSCS = Encoding.new("Big5-HKSCS") unless const_defined?(:BIG5_HKSCS, false)
  BIG5_HKSCS_2008 = BIG5_HKSCS unless const_defined?(:BIG5_HKSCS_2008, false)
  Big5_HKSCS = BIG5_HKSCS unless const_defined?(:Big5_HKSCS, false)
  Big5_HKSCS_2008 = BIG5_HKSCS unless const_defined?(:Big5_HKSCS_2008, false)
  BIG5_UAO = Encoding.new("Big5-UAO") unless const_defined?(:BIG5_UAO, false)
  Big5_UAO = BIG5_UAO unless const_defined?(:Big5_UAO, false)
  CESU_8 = Encoding.new("CESU-8") unless const_defined?(:CESU_8, false)
  CP50220 = Encoding.new("CP50220") unless const_defined?(:CP50220, false)
  CP50221 = Encoding.new("CP50221") unless const_defined?(:CP50221, false)
  CP51932 = Encoding.new("CP51932") unless const_defined?(:CP51932, false)
  CP850 = Encoding.new("CP850") unless const_defined?(:CP850, false)
  IBM850 = CP850 unless const_defined?(:IBM850, false)
  CP852 = Encoding.new("CP852") unless const_defined?(:CP852, false)
  CP855 = Encoding.new("CP855") unless const_defined?(:CP855, false)
  CP949 = Encoding.new("CP949") unless const_defined?(:CP949, false)
  CP950 = Encoding.new("CP950") unless const_defined?(:CP950, false)
  CP951 = Encoding.new("CP951") unless const_defined?(:CP951, false)
  EUC_JISX0213 = Encoding.new("EUC-JIS-2004") unless const_defined?(:EUC_JISX0213, false)
  EUC_JIS_2004 = EUC_JISX0213 unless const_defined?(:EUC_JIS_2004, false)
  EUCJP = Encoding.new("EUC-JP") unless const_defined?(:EUCJP, false)
  EUC_JP = EUCJP unless const_defined?(:EUC_JP, false)
  EucJP = EUCJP unless const_defined?(:EucJP, false)
  EUCKR = Encoding.new("EUC-KR") unless const_defined?(:EUCKR, false)
  EUC_KR = EUCKR unless const_defined?(:EUC_KR, false)
  EucKR = EUCKR unless const_defined?(:EucKR, false)
  EUCTW = Encoding.new("EUC-TW") unless const_defined?(:EUCTW, false)
  EUC_TW = EUCTW unless const_defined?(:EUC_TW, false)
  EucTW = EUCTW unless const_defined?(:EucTW, false)
  EMACS_MULE = Encoding.new("Emacs-Mule") unless const_defined?(:EMACS_MULE, false)
  Emacs_Mule = EMACS_MULE unless const_defined?(:Emacs_Mule, false)
  GB12345 = Encoding.new("GB12345") unless const_defined?(:GB12345, false)
  GB18030 = Encoding.new("GB18030") unless const_defined?(:GB18030, false)
  GB1988 = Encoding.new("GB1988") unless const_defined?(:GB1988, false)
  EUCCN = Encoding.new("GB2312") unless const_defined?(:EUCCN, false)
  EUC_CN = EUCCN unless const_defined?(:EUC_CN, false)
  EucCN = EUCCN unless const_defined?(:EucCN, false)
  GB2312 = EUCCN unless const_defined?(:GB2312, false)
  CP936 = Encoding.new("GBK") unless const_defined?(:CP936, false)
  GBK = CP936 unless const_defined?(:GBK, false)
  EBCDIC_CP_US = Encoding.new("IBM037") unless const_defined?(:EBCDIC_CP_US, false)
  IBM037 = EBCDIC_CP_US unless const_defined?(:IBM037, false)
  CP437 = Encoding.new("IBM437") unless const_defined?(:CP437, false)
  IBM437 = CP437 unless const_defined?(:IBM437, false)
  CP720 = Encoding.new("IBM720") unless const_defined?(:CP720, false)
  IBM720 = CP720 unless const_defined?(:IBM720, false)
  CP737 = Encoding.new("IBM737") unless const_defined?(:CP737, false)
  IBM737 = CP737 unless const_defined?(:IBM737, false)
  CP775 = Encoding.new("IBM775") unless const_defined?(:CP775, false)
  IBM775 = CP775 unless const_defined?(:IBM775, false)
  IBM852 = Encoding.new("IBM852") unless const_defined?(:IBM852, false)
  IBM855 = Encoding.new("IBM855") unless const_defined?(:IBM855, false)
  CP857 = Encoding.new("IBM857") unless const_defined?(:CP857, false)
  IBM857 = CP857 unless const_defined?(:IBM857, false)
  CP860 = Encoding.new("IBM860") unless const_defined?(:CP860, false)
  IBM860 = CP860 unless const_defined?(:IBM860, false)
  CP861 = Encoding.new("IBM861") unless const_defined?(:CP861, false)
  IBM861 = CP861 unless const_defined?(:IBM861, false)
  CP862 = Encoding.new("IBM862") unless const_defined?(:CP862, false)
  IBM862 = CP862 unless const_defined?(:IBM862, false)
  CP863 = Encoding.new("IBM863") unless const_defined?(:CP863, false)
  IBM863 = CP863 unless const_defined?(:IBM863, false)
  CP864 = Encoding.new("IBM864") unless const_defined?(:CP864, false)
  IBM864 = CP864 unless const_defined?(:IBM864, false)
  CP865 = Encoding.new("IBM865") unless const_defined?(:CP865, false)
  IBM865 = CP865 unless const_defined?(:IBM865, false)
  CP866 = Encoding.new("IBM866") unless const_defined?(:CP866, false)
  IBM866 = CP866 unless const_defined?(:IBM866, false)
  CP869 = Encoding.new("IBM869") unless const_defined?(:CP869, false)
  IBM869 = CP869 unless const_defined?(:IBM869, false)
  ISO2022_JP = Encoding.new("ISO-2022-JP") unless const_defined?(:ISO2022_JP, false)
  ISO_2022_JP = ISO2022_JP unless const_defined?(:ISO_2022_JP, false)
  ISO2022_JP2 = Encoding.new("ISO-2022-JP-2") unless const_defined?(:ISO2022_JP2, false)
  ISO_2022_JP_2 = ISO2022_JP2 unless const_defined?(:ISO_2022_JP_2, false)
  ISO_2022_JP_KDDI = Encoding.new("ISO-2022-JP-KDDI") unless const_defined?(:ISO_2022_JP_KDDI, false)
  ISO8859_1 = Encoding.new("ISO-8859-1") unless const_defined?(:ISO8859_1, false)
  ISO_8859_1 = ISO8859_1 unless const_defined?(:ISO_8859_1, false)
  ISO8859_10 = Encoding.new("ISO-8859-10") unless const_defined?(:ISO8859_10, false)
  ISO_8859_10 = ISO8859_10 unless const_defined?(:ISO_8859_10, false)
  ISO8859_11 = Encoding.new("ISO-8859-11") unless const_defined?(:ISO8859_11, false)
  ISO_8859_11 = ISO8859_11 unless const_defined?(:ISO_8859_11, false)
  ISO8859_13 = Encoding.new("ISO-8859-13") unless const_defined?(:ISO8859_13, false)
  ISO_8859_13 = ISO8859_13 unless const_defined?(:ISO_8859_13, false)
  ISO8859_14 = Encoding.new("ISO-8859-14") unless const_defined?(:ISO8859_14, false)
  ISO_8859_14 = ISO8859_14 unless const_defined?(:ISO_8859_14, false)
  ISO8859_15 = Encoding.new("ISO-8859-15") unless const_defined?(:ISO8859_15, false)
  ISO_8859_15 = ISO8859_15 unless const_defined?(:ISO_8859_15, false)
  ISO8859_16 = Encoding.new("ISO-8859-16") unless const_defined?(:ISO8859_16, false)
  ISO_8859_16 = ISO8859_16 unless const_defined?(:ISO_8859_16, false)
  ISO8859_2 = Encoding.new("ISO-8859-2") unless const_defined?(:ISO8859_2, false)
  ISO_8859_2 = ISO8859_2 unless const_defined?(:ISO_8859_2, false)
  ISO8859_3 = Encoding.new("ISO-8859-3") unless const_defined?(:ISO8859_3, false)
  ISO_8859_3 = ISO8859_3 unless const_defined?(:ISO_8859_3, false)
  ISO8859_4 = Encoding.new("ISO-8859-4") unless const_defined?(:ISO8859_4, false)
  ISO_8859_4 = ISO8859_4 unless const_defined?(:ISO_8859_4, false)
  ISO8859_5 = Encoding.new("ISO-8859-5") unless const_defined?(:ISO8859_5, false)
  ISO_8859_5 = ISO8859_5 unless const_defined?(:ISO_8859_5, false)
  ISO8859_6 = Encoding.new("ISO-8859-6") unless const_defined?(:ISO8859_6, false)
  ISO_8859_6 = ISO8859_6 unless const_defined?(:ISO_8859_6, false)
  ISO8859_7 = Encoding.new("ISO-8859-7") unless const_defined?(:ISO8859_7, false)
  ISO_8859_7 = ISO8859_7 unless const_defined?(:ISO_8859_7, false)
  ISO8859_8 = Encoding.new("ISO-8859-8") unless const_defined?(:ISO8859_8, false)
  ISO_8859_8 = ISO8859_8 unless const_defined?(:ISO_8859_8, false)
  ISO8859_9 = Encoding.new("ISO-8859-9") unless const_defined?(:ISO8859_9, false)
  ISO_8859_9 = ISO8859_9 unless const_defined?(:ISO_8859_9, false)
  CP878 = Encoding.new("KOI8-R") unless const_defined?(:CP878, false)
  KOI8_R = CP878 unless const_defined?(:KOI8_R, false)
  KOI8_U = Encoding.new("KOI8-U") unless const_defined?(:KOI8_U, false)
  MACJAPAN = Encoding.new("MacJapanese") unless const_defined?(:MACJAPAN, false)
  MACJAPANESE = MACJAPAN unless const_defined?(:MACJAPANESE, false)
  MacJapan = MACJAPAN unless const_defined?(:MacJapan, false)
  MacJapanese = MACJAPAN unless const_defined?(:MacJapanese, false)
  SJIS_DOCOMO = Encoding.new("SJIS-DoCoMo") unless const_defined?(:SJIS_DOCOMO, false)
  SJIS_DoCoMo = SJIS_DOCOMO unless const_defined?(:SJIS_DoCoMo, false)
  SJIS_KDDI = Encoding.new("SJIS-KDDI") unless const_defined?(:SJIS_KDDI, false)
  SJIS_SOFTBANK = Encoding.new("SJIS-SoftBank") unless const_defined?(:SJIS_SOFTBANK, false)
  SJIS_SoftBank = SJIS_SOFTBANK unless const_defined?(:SJIS_SoftBank, false)
  SHIFT_JIS = Encoding.new("Shift_JIS") unless const_defined?(:SHIFT_JIS, false)
  Shift_JIS = SHIFT_JIS unless const_defined?(:Shift_JIS, false)
  TIS_620 = Encoding.new("TIS-620") unless const_defined?(:TIS_620, false)
  ANSI_X3_4_1968 = Encoding.new("US-ASCII") unless const_defined?(:ANSI_X3_4_1968, false)
  ASCII = ANSI_X3_4_1968 unless const_defined?(:ASCII, false)
  US_ASCII = ANSI_X3_4_1968 unless const_defined?(:US_ASCII, false)
  UTF_16 = Encoding.new("UTF-16") unless const_defined?(:UTF_16, false)
  UCS_2BE = Encoding.new("UTF-16BE") unless const_defined?(:UCS_2BE, false)
  UTF_16BE = UCS_2BE unless const_defined?(:UTF_16BE, false)
  UTF_16LE = Encoding.new("UTF-16LE") unless const_defined?(:UTF_16LE, false)
  UTF_32 = Encoding.new("UTF-32") unless const_defined?(:UTF_32, false)
  UCS_4BE = Encoding.new("UTF-32BE") unless const_defined?(:UCS_4BE, false)
  UTF_32BE = UCS_4BE unless const_defined?(:UTF_32BE, false)
  UCS_4LE = Encoding.new("UTF-32LE") unless const_defined?(:UCS_4LE, false)
  UTF_32LE = UCS_4LE unless const_defined?(:UTF_32LE, false)
  CP65000 = Encoding.new("UTF-7") unless const_defined?(:CP65000, false)
  UTF_7 = CP65000 unless const_defined?(:UTF_7, false)
  CP65001 = Encoding.new("UTF-8") unless const_defined?(:CP65001, false)
  UTF_8 = CP65001 unless const_defined?(:UTF_8, false)
  UTF8_DOCOMO = Encoding.new("UTF8-DoCoMo") unless const_defined?(:UTF8_DOCOMO, false)
  UTF8_DoCoMo = UTF8_DOCOMO unless const_defined?(:UTF8_DoCoMo, false)
  UTF8_KDDI = Encoding.new("UTF8-KDDI") unless const_defined?(:UTF8_KDDI, false)
  UTF8_MAC = Encoding.new("UTF8-MAC") unless const_defined?(:UTF8_MAC, false)
  UTF_8_HFS = UTF8_MAC unless const_defined?(:UTF_8_HFS, false)
  UTF_8_MAC = UTF8_MAC unless const_defined?(:UTF_8_MAC, false)
  UTF8_SOFTBANK = Encoding.new("UTF8-SoftBank") unless const_defined?(:UTF8_SOFTBANK, false)
  UTF8_SoftBank = UTF8_SOFTBANK unless const_defined?(:UTF8_SoftBank, false)
  CP1250 = Encoding.new("Windows-1250") unless const_defined?(:CP1250, false)
  WINDOWS_1250 = CP1250 unless const_defined?(:WINDOWS_1250, false)
  Windows_1250 = CP1250 unless const_defined?(:Windows_1250, false)
  CP1251 = Encoding.new("Windows-1251") unless const_defined?(:CP1251, false)
  WINDOWS_1251 = CP1251 unless const_defined?(:WINDOWS_1251, false)
  Windows_1251 = CP1251 unless const_defined?(:Windows_1251, false)
  CP1252 = Encoding.new("Windows-1252") unless const_defined?(:CP1252, false)
  WINDOWS_1252 = CP1252 unless const_defined?(:WINDOWS_1252, false)
  Windows_1252 = CP1252 unless const_defined?(:Windows_1252, false)
  CP1253 = Encoding.new("Windows-1253") unless const_defined?(:CP1253, false)
  WINDOWS_1253 = CP1253 unless const_defined?(:WINDOWS_1253, false)
  Windows_1253 = CP1253 unless const_defined?(:Windows_1253, false)
  CP1254 = Encoding.new("Windows-1254") unless const_defined?(:CP1254, false)
  WINDOWS_1254 = CP1254 unless const_defined?(:WINDOWS_1254, false)
  Windows_1254 = CP1254 unless const_defined?(:Windows_1254, false)
  CP1255 = Encoding.new("Windows-1255") unless const_defined?(:CP1255, false)
  WINDOWS_1255 = CP1255 unless const_defined?(:WINDOWS_1255, false)
  Windows_1255 = CP1255 unless const_defined?(:Windows_1255, false)
  CP1256 = Encoding.new("Windows-1256") unless const_defined?(:CP1256, false)
  WINDOWS_1256 = CP1256 unless const_defined?(:WINDOWS_1256, false)
  Windows_1256 = CP1256 unless const_defined?(:Windows_1256, false)
  CP1257 = Encoding.new("Windows-1257") unless const_defined?(:CP1257, false)
  WINDOWS_1257 = CP1257 unless const_defined?(:WINDOWS_1257, false)
  Windows_1257 = CP1257 unless const_defined?(:Windows_1257, false)
  CP1258 = Encoding.new("Windows-1258") unless const_defined?(:CP1258, false)
  WINDOWS_1258 = CP1258 unless const_defined?(:WINDOWS_1258, false)
  Windows_1258 = CP1258 unless const_defined?(:Windows_1258, false)
  CP932 = Encoding.new("Windows-31J") unless const_defined?(:CP932, false)
  CSWINDOWS31J = CP932 unless const_defined?(:CSWINDOWS31J, false)
  CsWindows31J = CP932 unless const_defined?(:CsWindows31J, false)
  PCK = CP932 unless const_defined?(:PCK, false)
  SJIS = CP932 unless const_defined?(:SJIS, false)
  WINDOWS_31J = CP932 unless const_defined?(:WINDOWS_31J, false)
  Windows_31J = CP932 unless const_defined?(:Windows_31J, false)
  CP874 = Encoding.new("Windows-874") unless const_defined?(:CP874, false)
  WINDOWS_874 = CP874 unless const_defined?(:WINDOWS_874, false)
  Windows_874 = CP874 unless const_defined?(:Windows_874, false)
  EUCJP_MS = Encoding.new("eucJP-ms") unless const_defined?(:EUCJP_MS, false)
  EUC_JP_MS = EUCJP_MS unless const_defined?(:EUC_JP_MS, false)
  EucJP_ms = EUCJP_MS unless const_defined?(:EucJP_ms, false)
  MACCENTEURO = Encoding.new("macCentEuro") unless const_defined?(:MACCENTEURO, false)
  MacCentEuro = MACCENTEURO unless const_defined?(:MacCentEuro, false)
  MACCROATIAN = Encoding.new("macCroatian") unless const_defined?(:MACCROATIAN, false)
  MacCroatian = MACCROATIAN unless const_defined?(:MacCroatian, false)
  MACCYRILLIC = Encoding.new("macCyrillic") unless const_defined?(:MACCYRILLIC, false)
  MacCyrillic = MACCYRILLIC unless const_defined?(:MacCyrillic, false)
  MACGREEK = Encoding.new("macGreek") unless const_defined?(:MACGREEK, false)
  MacGreek = MACGREEK unless const_defined?(:MacGreek, false)
  MACICELAND = Encoding.new("macIceland") unless const_defined?(:MACICELAND, false)
  MacIceland = MACICELAND unless const_defined?(:MacIceland, false)
  MACROMAN = Encoding.new("macRoman") unless const_defined?(:MACROMAN, false)
  MacRoman = MACROMAN unless const_defined?(:MacRoman, false)
  MACROMANIA = Encoding.new("macRomania") unless const_defined?(:MACROMANIA, false)
  MacRomania = MACROMANIA unless const_defined?(:MacRomania, false)
  MACTHAI = Encoding.new("macThai") unless const_defined?(:MACTHAI, false)
  MacThai = MACTHAI unless const_defined?(:MacThai, false)
  MACTURKISH = Encoding.new("macTurkish") unless const_defined?(:MACTURKISH, false)
  MacTurkish = MACTURKISH unless const_defined?(:MacTurkish, false)
  MACUKRAINE = Encoding.new("macUkraine") unless const_defined?(:MACUKRAINE, false)
  MacUkraine = MACUKRAINE unless const_defined?(:MacUkraine, false)
  STATELESS_ISO_2022_JP = Encoding.new("stateless-ISO-2022-JP") unless const_defined?(:STATELESS_ISO_2022_JP, false)
  Stateless_ISO_2022_JP = STATELESS_ISO_2022_JP unless const_defined?(:Stateless_ISO_2022_JP, false)
  STATELESS_ISO_2022_JP_KDDI = Encoding.new("stateless-ISO-2022-JP-KDDI") unless const_defined?(:STATELESS_ISO_2022_JP_KDDI, false)
  Stateless_ISO_2022_JP_KDDI = STATELESS_ISO_2022_JP_KDDI unless const_defined?(:Stateless_ISO_2022_JP_KDDI, false)
  @@default_external = UTF_8
  @@default_internal = nil
  def self.default_external; @@default_external; end
  # Both setters take an Encoding, a String, or anything with #to_str; anything
  # else is a TypeError, and default_external may not be nil (CRuby).
  def self.__to_enc(v, what)
    return v if v.is_a?(Encoding)
    return find(v) if v.is_a?(String)
    unless v.respond_to?(:to_str)
      raise TypeError, "no implicit conversion of #{v.nil? ? 'nil' : v.class} into String"
    end
    n = v.to_str
    raise TypeError, "no implicit conversion of #{v.class} into String" unless n.is_a?(String)
    find(n)
  end
  def self.default_external=(e)
    raise ArgumentError, "default external cannot be nil" if e.nil?
    @@default_external = __to_enc(e, :external)
  end
  def self.default_internal; @@default_internal; end
  def self.default_internal=(e); @@default_internal = (e.nil? ? nil : __to_enc(e, :internal)); end
  # Encoding.find(name) → the Encoding constant whose #name matches (case-folded),
  # else a fresh Encoding of that name (used by String#encoding for "other").
  def self.find(name)
    # CRuby takes an Encoding, a String, or anything with #to_str — a Symbol is
    # a TypeError, not a name.
    n = if name.is_a?(Encoding) then name.name
        elsif name.is_a?(String) then name
        elsif name.respond_to?(:to_str) then String.try_convert(name) || name.to_str
        else raise TypeError, "no implicit conversion of #{name.class} into String"
        end
    return default_external if n == 'external' || n == 'filesystem' || n == 'locale'
    return default_internal if n == 'internal'
    # Resolve an alias first, so find("BINARY") is Encoding::ASCII_8BIT itself
    # rather than a fresh Encoding that merely prints the same.
    aliases.each { |a, canon| (n = canon; break) if a.upcase == n.upcase }
    constants.each do |cn|
      e = const_get(cn)
      return e if e.is_a?(Encoding) && (e.name == n || e.name.upcase == n.upcase)
    end
    raise ArgumentError, "unknown encoding name - #{n}"
  end
  # Like find, but nil instead of raising (encode turns a miss into
  # ConverterNotFoundError, which is a different exception class).
  def self.__find_or_nil(name)
    find(name)
  rescue ArgumentError
    nil
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
    sa = a.is_a?(String) ? a : (a.is_a?(Symbol) ? a.to_s : nil)
    sb = b.is_a?(String) ? b : (b.is_a?(Symbol) ? b.to_s : nil)
    # An empty operand is decided before the ASCII-compatibility gate, as in
    # rb_enc_compatible: it carries no bytes that could conflict.
    return ea if sb && sb.empty?
    return ((ea.ascii_compatible? && sb && sb.ascii_only?) ? ea : eb) if sa && sa.empty?
    # Anything else needs both sides to be ASCII-compatible.
    return nil unless ea.ascii_compatible? && eb.ascii_compatible?
    # "ASCII-only" coderange: a String/Symbol exposes it directly; a bare Encoding
    # has no content, so only US-ASCII (which can hold nothing but ASCII) counts.
    a1 = sa ? sa.ascii_only? : ea == US_ASCII
    b1 = sb ? sb.ascii_only? : eb == US_ASCII
    if b1 then ea
    elsif a1 then eb
    else nil
    end
  end
end
class Encoding
  # 実際の transcoding は未実装だが、フラグ定数は CRuby と同じ値を持たせて
  # おく (ライブラリが Encoding::Converter::UNDEF_REPLACE 等を参照するだけの
  # ケースが通る)。
  class Converter
    INVALID_MASK                = 0x0f
    INVALID_REPLACE             = 0x02
    UNDEF_MASK                  = 0xf0
    UNDEF_REPLACE               = 0x20
    UNDEF_HEX_CHARREF           = 0x30
    PARTIAL_INPUT               = 0x20000
    AFTER_OUTPUT                = 0x40000
    UNIVERSAL_NEWLINE_DECORATOR = 0x100
    CRLF_NEWLINE_DECORATOR      = 0x1000
    CR_NEWLINE_DECORATOR        = 0x2000
    LF_NEWLINE_DECORATOR        = 0x4000
    XML_TEXT_DECORATOR          = 0x8000
    XML_ATTR_CONTENT_DECORATOR  = 0x10000
    XML_ATTR_QUOTE_DECORATOR    = 0x100000

    attr_reader :source_encoding, :destination_encoding, :replacement

    def self.__encname(v, what)
      return v.name if v.is_a?(Encoding)
      return v if v.is_a?(String)
      raise TypeError, "no implicit conversion of #{v.class} into String" unless v.respond_to?(:to_str)
      n = v.to_str
      raise TypeError, "no implicit conversion of #{v.class} into String" unless n.is_a?(String)
      n
    end

    def initialize(src, dst, opts = 0)
      sname = Converter.__encname(src, :source)
      dname = Converter.__encname(dst, :destination)
      senc = Encoding.__find_or_nil(sname)
      denc = Encoding.__find_or_nil(dname)
      if senc.nil? || denc.nil? || senc == denc || !__transcodable?(sname) || !__transcodable?(dname)
        raise Encoding::ConverterNotFoundError, "code converter not found (#{sname} to #{dname})"
      end
      @source_encoding = senc
      @destination_encoding = denc
      @src_name = senc.name
      @dst_name = denc.name
      @flags = 0
      repl = nil
      if opts.is_a?(Hash)
        @flags |= 1 if (opts[:invalid] == :replace) || opts[:invalid_replace]
        @flags |= 2 if (opts[:undef] == :replace) || opts[:undef_replace]
        repl = opts[:replace]
      elsif opts.is_a?(Integer)
        @flags |= 1 if (opts & INVALID_MASK) == INVALID_REPLACE
        @flags |= 2 if (opts & UNDEF_MASK) == UNDEF_REPLACE
      end
      if repl.nil?
        @replacement = @dst_name == "UTF-8" ? "\u{fffd}".dup.force_encoding("UTF-8") : "?".dup.force_encoding("US-ASCII")
      else
        raise TypeError, "no implicit conversion of #{repl.class} into String" unless repl.respond_to?(:to_str)
        r = repl.to_str
        raise TypeError, "no implicit conversion of #{repl.class} into String" unless r.is_a?(String)
        @replacement = r
      end
      @pending = +""
      @finished = false
      @errinfo = [:source_buffer_empty, nil, nil, nil, nil]
      @last_error = nil
    end

    def replacement=(v)
      raise TypeError, "no implicit conversion of #{v.class} into String" unless v.respond_to?(:to_str)
      r = v.to_str
      raise TypeError, "no implicit conversion of #{v.class} into String" unless r.is_a?(String)
      @replacement = r
    end

    def inspect = "#<Encoding::Converter: #{@src_name} to #{@dst_name}>"
    def ==(o) = o.is_a?(Converter) && o.source_encoding == @source_encoding && o.destination_encoding == @destination_encoding
    def primitive_errinfo = @errinfo
    def last_error = @last_error
    def convpath = [[@source_encoding, @destination_encoding]]
    def self.search_convpath(src, dst, _opts = nil)
      [[Encoding.find(__encname(src, :source)), Encoding.find(__encname(dst, :destination))]]
    end
    def self.asciicompat_encoding(enc)
      e = enc.is_a?(Encoding) ? enc : Encoding.__find_or_nil(__encname(enc, :source))
      return nil if e.nil?
      e.ascii_compatible? ? nil : Encoding::UTF_8
    end

    # The replacement the primitive inserts has to already be in the target
    # encoding (it is spliced into the byte stream verbatim).
    def __repl_bytes
      return @replacement if @replacement.encoding.name.casecmp(@dst_name) == 0
      r = __transcode(@replacement, @replacement.encoding.name, @dst_name, 0, "")
      r.is_a?(String) ? r : "?"
    end
    private :__repl_bytes

    def primitive_convert(src, dst, dst_offset = nil, dst_bytesize = nil, opts = nil)
      raise FrozenError, "can't modify frozen String: #{dst.inspect}" if dst.frozen?
      off = dst_offset.nil? ? dst.bytesize : dst_offset.to_int
      raise ArgumentError, "too big destination byte offset" if off > dst.bytesize
      max = dst_bytesize.nil? ? -1 : dst_bytesize.to_int
      partial = opts.is_a?(Hash) ? !!opts[:partial_input] : false
      input = @pending + (src.nil? ? "" : src.dup.force_encoding(@src_name))
      @pending = +""
      r = __transcode(input, @src_name, @dst_name, @flags, __repl_bytes, max)
      if r.is_a?(String)
        out, code, consumed, cp, errb = r, nil, input.bytesize, nil, nil
      else
        out, code, consumed, cp, errb = r
      end
      dst.replace(dst.byteslice(0, off).to_s + out)
      dst.force_encoding(@dst_name)
      src.replace(+"") if src.is_a?(String) && !src.frozen?
      rest = input.byteslice(consumed + (errb ? errb.bytesize : 0), input.bytesize).to_s
      case code
      when nil
        @errinfo = [:finished, nil, nil, nil, nil]; @last_error = nil
        :finished
      when 0
        again = __read_again(input, consumed, errb)
        stage_dst = __decode_stage_dst
        @errinfo = [:invalid_byte_sequence, @src_name.b, stage_dst.b, errb.b, again.b]
        @last_error = Encoding::InvalidByteSequenceError.new("#{errb.b.inspect} followed by #{again.b.inspect} on #{@src_name}")
                        .__set_info(@src_name, stage_dst, errb.b, again.b, false)
        @putback_buf = again.b
        src.replace(rest.byteslice(again.bytesize, rest.bytesize).to_s) if src.is_a?(String) && !src.frozen?
        @pending = +"" if src.is_a?(String)
        :invalid_byte_sequence
      when 1
        @errinfo = [:undefined_conversion, "UTF-8".b, @dst_name.b, __err_char(errb, cp).b, +"".b]
        @last_error = Encoding::UndefinedConversionError.new(format("U+%04X from %s to %s", cp, @src_name, @dst_name))
                        .__set_info("UTF-8", @dst_name, __err_char(errb, cp))
        src.replace(rest) if src.is_a?(String) && !src.frozen?
        :undefined_conversion
      when 2
        @pending = input.byteslice(consumed, input.bytesize).to_s
        @errinfo = [:destination_buffer_full, nil, nil, nil, nil]; @last_error = nil
        :destination_buffer_full
      else
        if partial
          @pending = input.byteslice(consumed, input.bytesize).to_s
          @errinfo = [:source_buffer_empty, nil, nil, nil, nil]; @last_error = nil
          :source_buffer_empty
        else
          @errinfo = [:incomplete_input, @src_name.b, __decode_stage_dst.b, errb.b, +"".b]
          @last_error = Encoding::InvalidByteSequenceError.new("incomplete #{errb.b.inspect} on #{@src_name}")
                          .__set_info(@src_name, __decode_stage_dst, errb.b, nil, true)
          :incomplete_input
        end
      end
    end

    # An invalid byte sequence is found while DECODING, i.e. on the src -> UTF-8
    # leg of the pivot; CRuby names that leg's destination.  With a UTF-8 source
    # there is no such leg and the real destination is reported.
    def __decode_stage_dst
      @src_name.upcase == "UTF-8" ? @dst_name : "UTF-8"
    end
    private :__decode_stage_dst

    # CRuby reports the byte it had to read (and put back) to decide the
    # sequence was invalid: only when the error bytes were a lead expecting more.
    def __read_again(input, consumed, errb)
      nxt = input.byteslice(consumed + errb.bytesize, 1)
      return +"" if nxt.nil? || nxt.empty?
      b = errb.getbyte(0)
      return +"" if b.nil? || b < 0x80
      unless @src_name.upcase == "UTF-8"
        return errb.bytesize == 1 ? nxt.b : +""     # a lead byte of any multi-byte encoding
      end
      return +"" if b < 0xC0
      need = b < 0xE0 ? 2 : (b < 0xF0 ? 3 : 4)
      errb.bytesize < need ? nxt.b : +""
    end
    private :__read_again

    # An undefined conversion is always detected on the Unicode side of the
    # pivot, so CRuby reports UTF-8 as the failing stage's source encoding and
    # the offending character in UTF-8 (ISO-8859-1 -> UTF-8 -> EUC-JP etc.).
    def __err_char(errb, cp)
      return cp.chr(Encoding::UTF_8) if cp
      (errb || +"").dup.force_encoding(Encoding::UTF_8)
    end
    private :__err_char

    def convert(str)
      raise ArgumentError, "converter already finished" if @finished
      input = @pending + str.dup.force_encoding(@src_name)
      @pending = +""
      r = __transcode(input, @src_name, @dst_name, @flags, __repl_bytes, -1)
      if r.is_a?(String)
        @errinfo = [:source_buffer_empty, nil, nil, nil, nil]; @last_error = nil
        return r.force_encoding(@dst_name)
      end
      out, code, consumed, cp, errb = r
      case code
      when 0
        again = __read_again(input, consumed, errb)
        stage_dst = __decode_stage_dst
        @errinfo = [:invalid_byte_sequence, @src_name.b, stage_dst.b, errb.b, again.b]
        e = Encoding::InvalidByteSequenceError.new("#{errb.b.inspect} followed by #{again.b.inspect} on #{@src_name}")
              .__set_info(@src_name, stage_dst, errb.b, again.b, false)
        @last_error = e
        raise e
      when 1
        @errinfo = [:undefined_conversion, "UTF-8".b, @dst_name.b, __err_char(errb, cp).b, +"".b]
        e = Encoding::UndefinedConversionError.new(format("U+%04X from %s to %s", cp, @src_name, @dst_name))
              .__set_info("UTF-8", @dst_name, __err_char(errb, cp))
        @last_error = e
        raise e
      else                                          # truncated tail: keep it for the next call
        @pending = input.byteslice(consumed, input.bytesize).to_s
        @errinfo = [:source_buffer_empty, nil, nil, nil, nil]; @last_error = nil
        out.force_encoding(@dst_name)
      end
    end

    def finish
      @finished = true
      r = @pending.empty? ? +"" : convert_finish_pending
      @pending = +""
      r.force_encoding(@dst_name)
    end
    def convert_finish_pending
      r = __transcode(@pending, @src_name, @dst_name, @flags, __repl_bytes, -1)
      r.is_a?(String) ? r : (r[0] || +"")
    end
    private :convert_finish_pending

    def insert_output(str)
      @pending_out = (@pending_out || +"") + str.to_s
      nil
    end
    # After an :invalid_byte_sequence the read-ahead bytes are dropped from the
    # source; #putback hands them back so conversion can resume from them.
    def putback(n = nil)
      buf = @putback_buf || +""
      cnt = n.nil? ? buf.bytesize : n.to_int
      cnt = buf.bytesize if cnt > buf.bytesize
      r = buf.byteslice(0, cnt).to_s
      @putback_buf = buf.byteslice(cnt, buf.bytesize).to_s
      r.force_encoding(@src_name)
    end
  end
  class CompatibilityError < EncodingError; end
  class ConverterNotFoundError < EncodingError; end

  # The conversion-failure pair carries the transcoding stage that failed:
  # which encodings were in play and the offending character / bytes.
  class UndefinedConversionError < EncodingError
    attr_reader :source_encoding_name, :destination_encoding_name, :error_char
    def source_encoding = Encoding.find(@source_encoding_name)
    def destination_encoding = Encoding.find(@destination_encoding_name)
    def __set_info(src, dst, char)   # :nodoc:
      @source_encoding_name = src
      @destination_encoding_name = dst
      @error_char = char
      self
    end
  end

  class InvalidByteSequenceError < EncodingError
    attr_reader :source_encoding_name, :destination_encoding_name, :error_bytes, :readagain_bytes
    def source_encoding = Encoding.find(@source_encoding_name)
    def destination_encoding = Encoding.find(@destination_encoding_name)
    def incomplete_input? = !!@incomplete_input
    def __set_info(src, dst, errb, again, incomplete)   # :nodoc:
      @source_encoding_name = src
      @destination_encoding_name = dst
      @error_bytes = errb
      @readagain_bytes = (again.nil? || again.empty?) ? nil : again
      @incomplete_input = incomplete
      self
    end
  end
end
class String
  # Unicode normalization lives in lib/unicode_normalize (CRuby's own pure-Ruby
  # implementation, vendored); it is loaded on first use like CRuby does.
  def unicode_normalize(form = :nfc)
    require "unicode_normalize/normalize"
    UnicodeNormalize.normalize(self, form)
  end
  def unicode_normalize!(form = :nfc) = replace(unicode_normalize(form))
  def unicode_normalized?(form = :nfc)
    require "unicode_normalize/normalize"
    UnicodeNormalize.normalized?(self, form)
  end

  # scrub! — scrub を self に反映。変化がなくても self を返す (CRuby)。
  def scrub!(repl = nil, &block)
    r = repl.nil? ? scrub(&block) : scrub(repl, &block)
    replace(r)
    self
  end

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

  # encode([to][, from][, opts]) — real byte-level transcoding via __transcode
  # (builtins/transcode.c).  Encodings koruby has no converter for still convert
  # when the content is 7-bit, exactly as CRuby does.
  def __enc_arg(v)                            # Encoding / String / #to_str → name
    return nil if v.nil?
    return v.name if v.is_a?(Encoding)
    return v if v.is_a?(String)
    raise TypeError, "no implicit conversion of #{v.class} into String" unless v.respond_to?(:to_str)
    n = v.to_str
    raise TypeError, "no implicit conversion of #{v.class} into String" unless n.is_a?(String)
    n
  end
  private :__enc_arg

  def encode(*args)
    opts = args.last.is_a?(Hash) ? args.pop : {}
    to_name   = __enc_arg(args[0])
    from_name = __enc_arg(args[1]) || encoding.name
    if to_name.nil?
      di = Encoding.default_internal
      return __encode_decorate(dup, opts).force_encoding(from_name) if di.nil?
      to_name = di.name
    end
    tenc = Encoding.__find_or_nil(to_name)
    senc = Encoding.__find_or_nil(from_name)
    same = to_name.casecmp(from_name) == 0
    if (tenc.nil? || senc.nil?) && !same
      raise Encoding::ConverterNotFoundError, "code converter not found (#{from_name} to #{to_name})"
    end
    src = __encode_decorate(same ? dup : dup.force_encoding(from_name), opts)

    xml = opts[:xml]
    if xml
      raise ArgumentError, "unexpected value for xml option: #{xml}" unless xml == :text || xml == :attr
      src = src.gsub("&", "&amp;").gsub("<", "&lt;").gsub(">", "&gt;")
      src = %Q{"#{src.gsub('"', "&quot;")}"} if xml == :attr
    end

    # Same encoding on both sides: CRuby does no conversion at all — invalid
    # bytes survive — UNLESS invalid:/xml: asks for them to be rewritten.
    if same && !(opts[:invalid] == :replace || xml)
      return (tenc ? src.force_encoding(to_name) : src)
    end

    flags = 0
    flags |= 1 if opts[:invalid] == :replace
    flags |= 2 if opts[:undef] == :replace
    flags |= 4 if xml
    unless __transcodable?(to_name) && __transcodable?(from_name)
      # 7-bit content needs no converter at all (CRuby allows it)
      return src.force_encoding(to_name) if src.ascii_only? && tenc.ascii_compatible?
      raise Encoding::ConverterNotFoundError, "code converter not found (#{senc.name} to #{tenc.name})"
    end

    # The replacement is inserted verbatim, so it has to be in the TARGET
    # encoding already.  CRuby's default is U+FFFD where representable, "?" not.
    repl = opts[:replace]
    repl = repl.to_str if !repl.nil? && !repl.is_a?(String)
    if repl.nil?
      repl = __transcode("\uFFFD", "UTF-8", to_name, 0, "")
      repl = __transcode("?", "UTF-8", to_name, 0, "") unless repl.is_a?(String)
    elsif repl.encoding.name.casecmp(to_name) != 0
      conv = __transcode(repl, repl.encoding.name, to_name, 0, "")
      raise Encoding::UndefinedConversionError, "replacement not representable" unless conv.is_a?(String)
      repl = conv
    end

    fb = opts[:fallback]
    out = +""
    pos_src = src
    loop do
      r = __transcode(pos_src, from_name, to_name, flags, repl)
      if r.is_a?(String)
        out = out.empty? ? r : (out + r)
        break
      end
      partial, code, idx, cp, ch = r
      out += partial
      if code == 0 || code == 3
        raise Encoding::InvalidByteSequenceError.new("#{ch.b.inspect} on #{senc.name}")
                .__set_info(senc.name, tenc.name, ch.b, nil, code == 3)
      end
      rep = __encode_fallback(fb, ch)
      if rep.nil?
        raise Encoding::UndefinedConversionError
                .new(format("U+%04X from %s to %s", cp, senc.name, tenc.name))
                .__set_info("UTF-8", tenc.name, cp ? cp.chr(Encoding::UTF_8) : ch.dup.force_encoding(Encoding::UTF_8))
      end
      conv = __transcode(rep, rep.encoding.name, to_name, 0, repl)
      raise ArgumentError, "too big fallback string" unless conv.is_a?(String)
      out += conv
      pos_src = pos_src.byteslice(idx + ch.bytesize, pos_src.bytesize).force_encoding(from_name)
    end
    out.force_encoding(to_name)
  end

  # newline decorators, applied before the conversion (all ASCII edits)
  def __encode_decorate(r, opts)
    if opts[:universal_newline] then r.gsub(/\r\n|\r/, "\n")
    elsif opts[:cr_newline]     then r.gsub("\n", "\r")
    elsif opts[:crlf_newline]   then r.gsub("\n", "\r\n")
    else r
    end
  end
  private :__encode_decorate

  # `fallback:` lookup: Hash-like via #[], callable via #call, anything else is
  # "no replacement" (nil) and the caller raises UndefinedConversionError.
  def __encode_fallback(fb, ch)
    return nil if fb.nil?
    v = if fb.is_a?(Hash) || (!fb.respond_to?(:call) && fb.respond_to?(:[]))
          fb[ch]
        elsif fb.respond_to?(:call)
          fb.call(ch)
        else
          nil
        end
    return nil if v.nil?
    return v if v.is_a?(String)
    raise TypeError, "no implicit conversion of #{v.class} into String" unless v.respond_to?(:to_str)
    s = v.to_str
    raise TypeError, "no implicit conversion of #{v.class} into String" unless s.is_a?(String)
    s
  end
  private :__encode_fallback

  # #replace copies the source's bytes; take the target encoding from the
  # already-converted result rather than re-parsing the arguments.
  def encode!(*args, **opts)
    e = opts.empty? ? encode(*args) : encode(*args, **opts)
    enc = e.encoding
    replace(e)
    force_encoding(enc)
  end
end
class Symbol
  # A Symbol reports US-ASCII when its name is ASCII-only, else UTF-8 (CRuby).
  def encoding
    to_s.ascii_only? ? Encoding::US_ASCII : Encoding::UTF_8
  end
end
class Regexp
  # Encoding of the pattern.  koruby distinguishes only the ASCII-compatible
  # encodings (US-ASCII / UTF-8 / ASCII-8BIT); the /e (EUC-JP) and /s (Windows-31J)
  # source options need real transcoding and are out of scope.  A 7-bit-ASCII
  # pattern is US-ASCII; otherwise it carries its source String's encoding.
  def encoding
    src = source
    # A \u / \u{…} escape whose codepoint is outside 7-bit ASCII forces UTF-8,
    # even when the escape text itself is ASCII (CRuby).  \u escapes that stay in
    # 7-bit ASCII (e.g. A) do not.
    utf8 = src.scan(/\\u(?:\{([0-9a-fA-F][0-9a-fA-F ]*)\}|([0-9a-fA-F]{4}))/).any? do |brace, plain|
      (brace ? brace.split : [plain]).any? { |h| h.to_i(16) > 0x7f }
    end
    o = options
    if (o & 32) != 0                               # /n (NOENCODING)
      # a \xNN escape above 0x7f makes the pattern binary; otherwise it is
      # plain 7-bit and CRuby reports US-ASCII
      bin = src.scan(/\\x([0-9a-fA-F]{1,2})/).any? { |h| h[0].to_i(16) > 0x7f }
      return (bin || !src.ascii_only?) ? Encoding::ASCII_8BIT : Encoding::US_ASCII
    end
    # /e and /s fix the pattern to EUC-JP / Windows-31J; koruby does not
    # transcode, so only the reported encoding follows the modifier.
    h = __enc_hint
    return Encoding.find(h) if h
    return src.encoding unless src.encoding.ascii_compatible?   # UTF-16/32 … pin themselves
    return Encoding::UTF_8 if utf8
    return src.encoding if fixed_encoding?        # pinned: the source's own encoding
    src.ascii_only? ? Encoding::US_ASCII : src.encoding
  end
end
