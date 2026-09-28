# nkf — CRuby's is a C extension (nkf.so).  This is the common part of its
# API over String#encode: the output / input encoding flags of NKF.nkf and
# NKF.guess.  MIME, width and newline conversions are not implemented.
module NKF
  VERSION = "2.1.5 (2018-12-15)"
  NKF_VERSION = "2.1.5"
  NKF_RELEASE_DATE = "2018-12-15"

  AUTO    = nil
  NOCONV  = nil
  UNKNOWN = nil
  JIS     = Encoding::ISO_2022_JP
  EUC     = Encoding::EUC_JP
  SJIS    = Encoding::Shift_JIS
  BINARY  = Encoding::BINARY
  ASCII   = Encoding::US_ASCII
  UTF8    = Encoding::UTF_8
  UTF16   = Encoding::UTF_16BE
  UTF32   = Encoding::UTF_32BE

  OUT__ = { "w" => UTF8, "s" => SJIS, "e" => EUC, "j" => JIS }.freeze
  IN__  = { "W" => UTF8, "S" => SJIS, "E" => EUC, "J" => JIS }.freeze
  private_constant :OUT__, :IN__

  module_function

  def nkf(opt, str)
    raise TypeError, "no implicit conversion of #{opt.class} into String" unless opt.respond_to?(:to_str)
    str = str.to_str
    out = JIS                                          # nkf's default output
    inp = nil
    opt.to_str.split.each do |o|
      if (m = /\A--(ic|oc)=(.+)\z/.match(o))            # Kconv passes encodings by name
        e = Encoding.find(m[2]) rescue next
        m[1] == "ic" ? (inp = e) : (out = e)
        next
      end
      next unless o.start_with?("-") && !o.start_with?("--")
      o[1..].scan(/[a-zA-Z]\d*/) do |f|
        case f
        when /\Aw(16|32)?/ then out = $1 == "16" ? UTF16 : $1 == "32" ? UTF32 : UTF8
        when /\A[sej]\z/   then out = OUT__[f]
        when /\AW(16|32)?/ then inp = $1 == "16" ? UTF16 : $1 == "32" ? UTF32 : UTF8
        when /\A[SEJ]\z/   then inp = IN__[f]
        end
      end
    end
    inp ||= guess(str) || UTF8
    inp = BINARY if inp == ASCII && !str.ascii_only?
    s = str.dup.force_encoding(inp)
    s.encode(out, invalid: :replace, undef: :replace)
  end

  # the encoding the bytes look like: ASCII, then UTF-8, EUC-JP, Shift_JIS, ISO-2022-JP
  def guess(str)
    b = str.to_str.b
    return JIS if b.include?("\e$B") || b.include?("\e$@") || b.include?("\e(J")
    return ASCII if b.ascii_only?
    return UTF8 if b.dup.force_encoding(UTF8).valid_encoding?
    return EUC if b.dup.force_encoding(EUC).valid_encoding?
    return SJIS if b.dup.force_encoding(SJIS).valid_encoding?
    BINARY
  end
end
