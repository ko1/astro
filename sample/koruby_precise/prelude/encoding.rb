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
  def self.default_external=(e); @@default_external = e.is_a?(String) ? find(e) : e; end
  def self.default_internal; @@default_internal; end
  def self.default_internal=(e); @@default_internal = (e.nil? ? nil : (e.is_a?(String) ? find(e) : e)); end
  # Encoding.find(name) → the Encoding constant whose #name matches (case-folded),
  # else a fresh Encoding of that name (used by String#encoding for "other").
  def self.find(name)
    n = name.to_s
    return default_external if n == 'external' || n == 'filesystem' || n == 'locale'
    return (default_internal || default_external) if n == 'internal'
    # Resolve an alias first, so find("BINARY") is Encoding::ASCII_8BIT itself
    # rather than a fresh Encoding that merely prints the same.
    aliases.each { |a, canon| (n = canon; break) if a.upcase == n.upcase }
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
  end
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

  # encode([enc][, from][, opts]) — koruby models the 3 ASCII-compatible encodings,
  # where transcoding of ASCII-only data is a no-op (just a tag change).  Non-ASCII
  # content only converts trivially when the target encoding matches; US-ASCII
  # targets require ASCII-only content; non-ASCII-compatible targets (UTF-16/32)
  # and real byte transcoding are out of scope.
  def encode(*args)
    opts = args.last.is_a?(Hash) ? args.pop : {}
    tenc = args[0].nil? ? Encoding.default_external : (args[0].is_a?(Encoding) ? args[0] : Encoding.find(args[0].to_s))
    senc = args[1] ? (args[1].is_a?(Encoding) ? args[1] : Encoding.find(args[1].to_s)) : encoding
    replace_undef = [:undef, :invalid, :replace, :fallback, :xml].any? { |k| opts.key?(k) }
    r = dup
    if opts[:universal_newline]                   # \r\n and \r → \n
      r = r.gsub(/\r\n|\r/, "\n")
    elsif opts[:cr_newline]                       # \n → \r
      r = r.gsub("\n", "\r")
    elsif opts[:crlf_newline]                     # \n → \r\n
      r = r.gsub("\n", "\r\n")
    end
    if tenc == senc || (r.ascii_only? && tenc.ascii_compatible?)
      return r.force_encoding(tenc.name)          # identity / ASCII-only → tag change only
    end
    # US-ASCII / BINARY targets can't hold non-ASCII source content.
    if (tenc == Encoding::US_ASCII) && !ascii_only? && !replace_undef
      raise Encoding::UndefinedConversionError, "from #{senc.name} to US-ASCII"
    end
    # non-ASCII-compatible targets (UTF-16/32) need real byte transcoding, which is
    # out of scope — return a best-effort tag change rather than a spurious error.
    r.force_encoding(tenc.name)
  end
  def encode!(*args); replace(encode(*args)); force_encoding(args[0].is_a?(Encoding) ? args[0].name : (args[0] || Encoding.default_external.name).to_s); end
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
    return Encoding::UTF_8 if utf8
    src.ascii_only? ? Encoding::US_ASCII : src.encoding
  end
end
