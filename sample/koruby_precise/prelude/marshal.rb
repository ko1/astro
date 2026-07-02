# Marshal — CRuby-compatible (format 4.8) dump/load for the common types:
# nil/true/false, Integer, Float, String, Symbol, Array, Hash.  Pure Ruby.
# Round-trips within koruby and interoperates with CRuby for these types.
# (No object links, ivars, or user #marshal_dump yet.)
module Marshal
  MAJOR_VERSION = 4
  MINOR_VERSION = 8

  def self.dump(obj, _limit = nil)
    out = +"\x04\x08"
    _dump(obj, out)
    out
  end

  def self._dump(o, out)
    case o
    when nil            then out << "0"
    when true           then out << "T"
    when false          then out << "F"
    when Integer
      if o >= -0x40000000 && o < 0x40000000
        out << "i"; _long(o, out)            # compact long (fits in 4 bytes)
      else
        _bignum(o, out)                       # 'l' Bignum
      end
    when Symbol
      s = o.to_s
      out << ":"; _long(s.bytesize, out); out << s
    when String
      out << "\""; _long(o.bytesize, out); out << o
    when Float
      s = _float_str(o)
      out << "f"; _long(s.bytesize, out); out << s
    when Array
      out << "["; _long(o.length, out)
      o.each { |e| _dump(e, out) }
    when Hash
      out << "{"; _long(o.size, out)
      o.each { |k, v| _dump(k, out); _dump(v, out) }
    else
      raise TypeError, "can't dump #{o.class}"
    end
  end

  # CRuby's compact "long" (Fixnum) encoding.
  def self._long(n, out)
    if n == 0
      out << 0.chr
    elsif n > 0 && n < 123
      out << (n + 5).chr
    elsif n < 0 && n > -124
      out << ((n - 5) & 0xff).chr
    else
      bytes = []
      v = n
      if n > 0
        while v > 0
          bytes << (v & 0xff); v >>= 8
        end
        out << bytes.length.chr
      else
        while v < -1
          bytes << (v & 0xff); v >>= 8
        end
        out << ((-bytes.length) & 0xff).chr
      end
      bytes.each { |b| out << b.chr }
    end
  end

  # 'l' Bignum: sign byte, length in 2-byte words (as a long), then LE words.
  def self._bignum(n, out)
    out << "l"
    out << (n < 0 ? "-" : "+")
    mag = n.abs
    words = []
    while mag > 0
      words << (mag & 0xffff); mag >>= 16
    end
    words << 0 if words.empty?
    _long(words.length, out)
    words.each { |w| out << (w & 0xff).chr; out << ((w >> 8) & 0xff).chr }
  end

  def self._float_str(f)
    return "inf" if f == Float::INFINITY
    return "-inf" if f == -Float::INFINITY
    return "nan" if f.nan?
    s = f.to_s
    s = s[0..-3] if s.end_with?(".0")   # CRuby drops a trailing ".0"
    s
  end

  def self.load(data, _proc = nil)
    data = data.string if data.respond_to?(:string)
    st = { s: data, i: 0, syms: [] }
    maj = _byte(st); min = _byte(st)
    _read(st)
  end

  def self._byte(st)
    b = st[:s].getbyte(st[:i])
    st[:i] += 1
    b
  end

  def self._read(st)
    t = _byte(st)
    case t
    when 0x30 then nil                                  # '0'
    when 0x54 then true                                 # 'T'
    when 0x46 then false                                # 'F'
    when 0x69 then _rlong(st)                            # 'i'
    when 0x6c                                            # 'l' Bignum
      sign = _byte(st)
      nwords = _rlong(st)
      mag = 0
      nwords.times { |k| lo = _byte(st); hi = _byte(st); mag |= (lo | (hi << 8)) << (16 * k) }
      sign == 0x2d ? -mag : mag
    when 0x3a                                            # ':' symbol
      n = _rlong(st); sym = _bytes(st, n).to_sym
      st[:syms] << sym; sym
    when 0x3b then st[:syms][_rlong(st)]                # ';' symbol link
    when 0x22 then _bytes(st, _rlong(st))               # '"' string
    when 0x66                                           # 'f' float
      s = _bytes(st, _rlong(st))
      case s
      when "inf" then Float::INFINITY
      when "-inf" then -Float::INFINITY
      when "nan" then Float::NAN
      else s.to_f
      end
    when 0x5b                                            # '[' array
      n = _rlong(st); Array.new(n) { _read(st) }
    when 0x7b                                            # '{' hash
      n = _rlong(st); h = {}
      n.times { k = _read(st); h[k] = _read(st) }
      h
    when 0x49                                            # 'I' ivar-wrapped (skip ivars)
      v = _read(st)
      ni = _rlong(st)
      ni.times { _read(st); _read(st) }
      v
    else
      raise TypeError, "unsupported Marshal type 0x#{t.to_s(16)}"
    end
  end

  def self._bytes(st, n)
    r = st[:s].byteslice(st[:i], n)
    st[:i] += n
    r
  end

  def self._rlong(st)
    c = _byte(st)
    c = c - 256 if c > 127
    return 0 if c == 0
    if c > 0
      return c - 5 if c >= 5          # 5..127 → value 0..122
      n = 0                           # c in 1..4 → that many little-endian bytes
      c.times { |k| n |= _byte(st) << (8 * k) }
      n
    else
      return c + 5 if c <= -5         # -128..-5 → value -123..0
      cnt = -c                        # c in -4..-1 → that many bytes
      n = -1
      cnt.times { |k| n &= ~(0xff << (8 * k)); n |= _byte(st) << (8 * k) }
      n
    end
  end
end
