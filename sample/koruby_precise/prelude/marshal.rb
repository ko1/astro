# Marshal — CRuby-compatible (format 4.8) dump/load.
# Pure Ruby.  Byte-compatible with CRuby for the mainstream object graph:
# nil/true/false, Integer (Fixnum + Bignum), Float, String (with encoding),
# Symbol (with symbol links), Array, Hash, Range, Struct, Rational, Complex,
# Class ('c'), Module ('m'), user objects via #_dump ('u') / #marshal_dump ('U')
# and generic instance-variable objects ('o').  Object links ('@') and symbol
# links (';') are emitted for repeated objects, matching CRuby's tables.
module Marshal
  MAJOR_VERSION = 4
  MINOR_VERSION = 8

  def self.dump(obj, an_io = nil, limit = nil)
    if limit.nil? && an_io.is_a?(Integer)
      limit = an_io; an_io = nil
    end
    st = { syms: {}, objs: {}.compare_by_identity }
    out = +"\x04\x08"
    _dump(obj, out, st)
    out.force_encoding("ASCII-8BIT") if out.respond_to?(:force_encoding)
    if an_io && an_io.respond_to?(:write)
      an_io.write(out); an_io
    else
      out
    end
  end

  # --- dump ------------------------------------------------------------------

  def self._dump(o, out, st)
    case o
    when nil   then out << "0"; return
    when true  then out << "T"; return
    when false then out << "F"; return
    when Symbol then _symdump(o, out, st); return
    when Integer
      if o >= -0x40000000 && o < 0x40000000
        out << "i"; _long(o, out); return             # compact Fixnum
      end
      # Bignum is a linkable object — fall through
    end
    id = st[:objs][o]                                  # object link
    if id
      out << "@"; _long(id, out); return
    end
    st[:objs][o] = st[:objs].size                      # assign link id (pre-order)
    _dump_val(o, out, st)
  end

  def self._dump_val(o, out, st)
    case o
    when Class  then return _dump_class(o, out, st)    # (before String: Class subjects mis-match `when String`)
    when Module
      n = o.name
      raise TypeError, "can't dump anonymous module #{o}" if n.nil?
      out << "m"; _long(n.bytesize, out); out << n
      return
    end
    case o
    when Integer then _bignum(o, out)                  # Bignum only reaches here
    when String  then _dump_string(o, out, st)
    when Float
      s = _float_str(o)
      out << "f"; _long(s.bytesize, out); out << s
    when Regexp  then _dump_regexp(o, out, st)
    when Array   then _dump_array(o, out, st)
    when Hash    then _dump_hash(o, out, st)
    when Range                                         # generic object: class :Range + 3 ivars
      out << "o"
      _symdump(:Range, out, st); _long(3, out)
      _symdump(:excl, out, st);  _dump(o.exclude_end?, out, st)
      _symdump(:begin, out, st); _dump(o.begin, out, st)
      _symdump(:end, out, st);   _dump(o.end, out, st)
    when Struct
      name = o.class.name
      raise TypeError, "can't dump anonymous class #{o.class}" if name.nil?
      out << "S"
      _symdump(name.to_sym, out, st)
      mem = o.members; vals = o.to_a
      _long(mem.length, out)
      mem.each_with_index { |m, i| _symdump(m, out, st); _dump(vals[i], out, st) }
    when Rational                                      # 'U' user-marshal
      out << "U"; _symdump(:Rational, out, st); _dump([o.numerator, o.denominator], out, st)
    when Complex
      out << "U"; _symdump(:Complex, out, st); _dump([o.real, o.imaginary], out, st)
    else
      if defined?(Data) && Data === o
        _dump_data(o, out, st)
      elsif o.respond_to?(:marshal_dump)
        _dump_umarshal(o, out, st)
      elsif o.respond_to?(:_dump)
        _dump_udump(o, out, st)
      else
        _dump_generic(o, out, st)
      end
    end
  end

  # 'e' (extend module) and 'C' (subclass) prefixes, outermost after any 'I'.
  # Also enforces the TypeErrors CRuby raises for un-dumpable singletons.
  def self._wrap_prefix(o, base, out, st)
    unless (o.singleton_methods(false) rescue []).empty?
      raise TypeError, "singleton can't be dumped"
    end
    exts = (o.singleton_class.included_modules - o.class.included_modules rescue [])
    exts.each do |m|
      nm = m.name
      raise TypeError, "can't dump anonymous module #{m}" if nm.nil?
      out << "e"; _symdump(nm.to_sym, out, st)
    end
    if o.class != base
      nm = o.class.name
      raise TypeError, "can't dump anonymous class #{o.class}" if nm.nil?
      out << "C"; _symdump(nm.to_sym, out, st)
    end
  end

  def self._dump_array(o, out, st)
    uiv = o.instance_variables
    out << "I" unless uiv.empty?
    _wrap_prefix(o, Array, out, st)
    out << "["; _long(o.length, out)
    o.each { |e| _dump(e, out, st) }
    _dump_ivars(o, uiv, out, st) unless uiv.empty?
  end

  def self._dump_hash(o, out, st)
    raise TypeError, "can't dump hash with default proc" if o.default_proc
    uiv = o.instance_variables
    out << "I" unless uiv.empty?
    _wrap_prefix(o, Hash, out, st)
    if o.default.nil?
      out << "{"; _long(o.size, out)
    else
      out << "}"; _long(o.size, out)
    end
    o.each { |k, v| _dump(k, out, st); _dump(v, out, st) }
    _dump(o.default, out, st) unless o.default.nil?
    _dump_ivars(o, uiv, out, st) unless uiv.empty?
  end

  def self._dump_regexp(o, out, st)
    src = o.source
    out << "I"
    _wrap_prefix(o, Regexp, out, st)
    out << "/"; _long(src.bytesize, out); out << src
    out << (o.options & 0xff).chr
    uiv = o.instance_variables
    _long(1 + uiv.length, out)
    _symdump(:E, out, st); _dump(false, out, st)      # ascii regexp encoding
    uiv.each { |iv| _symdump(iv, out, st); _dump(o.instance_variable_get(iv), out, st) }
  end

  def self._dump_data(o, out, st)
    name = o.class.name
    raise TypeError, "can't dump anonymous class #{o.class}" if name.nil?
    _wrap_prefix(o, o.class, out, st)                  # 'e' extends only ('S' carries the name)
    out << "S"; _symdump(name.to_sym, out, st)
    mem = o.class.members
    _long(mem.length, out)
    mem.each { |m| _symdump(m, out, st); _dump(o.send(m), out, st) }
  end

  def self._dump_ivars(o, ivars, out, st)
    _long(ivars.length, out)
    ivars.each { |iv| _symdump(iv, out, st); _dump(o.instance_variable_get(iv), out, st) }
  end

  def self._dump_class(o, out, st)
    n = o.name
    raise TypeError, "can't dump anonymous class #{o}" if n.nil?
    raise TypeError, "singleton can't be dumped" if o.inspect.start_with?("#<Class:")
    out << "c"; _long(n.bytesize, out); out << n
  end

  def self._dump_umarshal(o, out, st)
    name = o.class.name
    raise TypeError, "can't dump anonymous class #{o.class}" if name.nil?
    d = o.marshal_dump
    out << "U"; _symdump(name.to_sym, out, st); _dump(d, out, st)
  end

  def self._dump_udump(o, out, st)
    name = o.class.name
    raise TypeError, "can't dump anonymous class #{o.class}" if name.nil?
    d = o._dump(-1)
    unless String === d
      raise TypeError, "_dump() must return string, not #{d.class}"
    end
    enc = _str_enc_marker(d)
    if enc
      out << "I"
      out << "u"; _symdump(name.to_sym, out, st); _long(d.bytesize, out); out << d
      _long(1, out); _write_enc(enc, out, st)
    else
      out << "u"; _symdump(name.to_sym, out, st); _long(d.bytesize, out); out << d
    end
  end

  def self._dump_generic(o, out, st)
    name = o.class.name
    raise TypeError, "can't dump anonymous class #{o.class}" if name.nil?
    _wrap_prefix(o, o.class, out, st)                  # 'e' extends only ('o' carries the name)
    ivars = o.instance_variables
    out << "o"
    _symdump(name.to_sym, out, st); _long(ivars.length, out)
    ivars.each { |iv| _symdump(iv, out, st); _dump(o.instance_variable_get(iv), out, st) }
  end

  def self._dump_string(o, out, st)
    enc = _str_enc_marker(o)
    uiv = o.instance_variables
    if enc.nil? && uiv.empty? && o.class == String
      out << "\""; _long(o.bytesize, out); out << o
      return
    end
    out << "I" if enc || !uiv.empty?
    _wrap_prefix(o, String, out, st)
    out << "\""; _long(o.bytesize, out); out << o
    if enc || !uiv.empty?
      _long((enc ? 1 : 0) + uiv.length, out)
      _write_enc(enc, out, st) if enc
      uiv.each { |iv| _symdump(iv, out, st); _dump(o.instance_variable_get(iv), out, st) }
    end
  end

  # Encoding marker for a String, or nil for ASCII-8BIT (dumped bare).
  #   [:E, true]  → UTF-8      [:E, false] → US-ASCII
  #   [:encoding, name] → any other (transcoding domain)
  def self._str_enc_marker(s)
    case s.encoding.name
    when "UTF-8"      then [:E, true]
    when "US-ASCII"   then [:E, false]
    when "ASCII-8BIT" then nil
    else [:encoding, s.encoding.name]
    end
  end

  def self._write_enc(enc, out, st)
    if enc[0] == :E
      _symdump(:E, out, st); _dump(enc[1], out, st)
    else
      _symdump(:encoding, out, st)
      out << "\""; _long(enc[1].bytesize, out); out << enc[1]
    end
  end

  # Symbol: bare for ASCII, I-wrapped with encoding for non-ASCII; links on repeat.
  def self._symdump(sym, out, st)
    id = st[:syms][sym]
    if id
      out << ";"; _long(id, out); return
    end
    st[:syms][sym] = st[:syms].size
    s = sym.to_s
    if s.ascii_only?
      out << ":"; _long(s.bytesize, out); out << s     # ASCII symbol → bare
    else
      out << "I"; out << ":"; _long(s.bytesize, out); out << s
      _long(1, out); _write_enc(_str_enc_marker(s), out, st)
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

  # --- load ------------------------------------------------------------------

  def self.load(data, _proc = nil)
    data = data.read if data.respond_to?(:read) && !data.is_a?(String)
    data = data.string if data.respond_to?(:string)
    st = { s: data, i: 0, syms: [], objs: [] }
    _byte(st); _byte(st)                                # major, minor
    _read(st)
  end
  class << self; alias restore load; end

  def self._byte(st)
    b = st[:s].getbyte(st[:i])
    st[:i] += 1
    b
  end

  def self._reg(st, obj)                               # register a linkable object
    if (i = st[:reuse])
      st[:reuse] = nil                                 # a 'C'/'e' wrapper reserved this slot
      st[:objs][i] = obj
    else
      st[:objs] << obj
    end
    obj
  end

  def self._reclass(cls, inner)                        # rebuild inner as an instance of cls
    case inner
    when String
      cls.new(inner)
    when Array
      a = cls.new; a.replace(inner); a
    when Hash
      h = cls.new; inner.each { |k, v| h[k] = v }; h
    when Regexp
      cls.new(inner.source, inner.options)
    else inner
    end
  rescue StandardError
    inner
  end

  def self._read(st)
    t = _byte(st)
    case t
    when 0x30 then nil                                  # '0'
    when 0x54 then true                                 # 'T'
    when 0x46 then false                                # 'F'
    when 0x69 then _rlong(st)                            # 'i' Fixnum
    when 0x40 then st[:objs][_rlong(st)]                # '@' object link
    when 0x6c                                            # 'l' Bignum
      sign = _byte(st)
      nwords = _rlong(st)
      mag = 0
      nwords.times { |k| lo = _byte(st); hi = _byte(st); mag |= (lo | (hi << 8)) << (16 * k) }
      _reg(st, sign == 0x2d ? -mag : mag)
    when 0x3a                                            # ':' symbol
      n = _rlong(st); sym = _bytes(st, n).to_sym
      st[:syms] << sym; sym
    when 0x3b then st[:syms][_rlong(st)]                # ';' symbol link
    when 0x22                                            # '"' string
      _reg(st, _bytes(st, _rlong(st)))
    when 0x66                                            # 'f' Float
      s = _bytes(st, _rlong(st))
      v = case s
          when "inf" then Float::INFINITY
          when "-inf" then -Float::INFINITY
          when "nan" then Float::NAN
          else s.to_f
          end
      _reg(st, v)
    when 0x5b                                            # '[' array
      n = _rlong(st); a = []; _reg(st, a)
      n.times { a << _read(st) }
      a
    when 0x7b, 0x7d                                      # '{' hash / '}' hash-with-default
      n = _rlong(st); h = {}; _reg(st, h)
      n.times { k = _read(st); h[k] = _read(st) }
      h.default = _read(st) if t == 0x7d
      h
    when 0x49                                            # 'I' ivar-wrapped
      v = _read(st)
      ni = _rlong(st)
      ni.times do
        name = _read(st); val = _read(st)
        next if name == :E || name == :encoding         # encoding markers, not user ivars
        v.instance_variable_set(name, val) rescue nil
      end
      v
    when 0x2f                                            # '/' Regexp
      src = _bytes(st, _rlong(st)); opt = _byte(st)
      _reg(st, Regexp.new(src, opt))
    when 0x43                                            # 'C' subclass wrapper
      idx = st[:objs].size; st[:objs] << nil
      cls = Object.const_get(_read(st))
      st[:reuse] = idx
      inner = _read(st)
      obj = _reclass(cls, inner)
      st[:objs][idx] = obj
    when 0x65                                            # 'e' extend-module wrapper
      msym = _read(st)
      obj = _read(st)
      (obj.extend(Object.const_get(msym)) rescue nil)
      obj
    when 0x63                                            # 'c' Class
      _reg(st, Object.const_get(_bytes(st, _rlong(st))))
    when 0x6d                                            # 'm' Module
      _reg(st, Object.const_get(_bytes(st, _rlong(st))))
    when 0x6f                                            # 'o' generic object
      idx = st[:objs].size; st[:objs] << nil
      cls = _read(st)
      ni = _rlong(st)
      ivars = {}
      ni.times { name = _read(st); ivars[name] = _read(st) }
      if cls == :Range
        obj = Range.new(ivars[:begin], ivars[:end], ivars[:excl])
      else
        obj = Object.const_get(cls).allocate
        ivars.each { |name, val| obj.instance_variable_set(name, val) }
      end
      st[:objs][idx] = obj
    when 0x75                                            # 'u' user _dump
      idx = st[:objs].size; st[:objs] << nil
      cls = _read(st)
      data = _bytes(st, _rlong(st))
      obj = Object.const_get(cls)._load(data)
      st[:objs][idx] = obj
    when 0x53                                            # 'S' Struct
      idx = st[:objs].size; st[:objs] << nil
      cls = _read(st)
      n = _rlong(st)
      vals = []
      n.times { _read(st); vals << _read(st) }
      st[:objs][idx] = Object.const_get(cls).new(*vals)
    when 0x55                                            # 'U' user marshal_dump
      idx = st[:objs].size; st[:objs] << nil
      cls = _read(st)
      data = _read(st)
      obj = case cls
            when :Rational then Rational(data[0], data[1])
            when :Complex  then Complex(data[0], data[1])
            else
              o = Object.const_get(cls).allocate
              o.marshal_load(data) if o.respond_to?(:marshal_load)
              o
            end
      st[:objs][idx] = obj
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
