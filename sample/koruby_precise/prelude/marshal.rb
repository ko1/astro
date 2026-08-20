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
    raise TypeError, "instance of IO needed" if an_io && !an_io.respond_to?(:write)
    st = { syms: {}, objs: {}.compare_by_identity, limit: limit, depth: 0 }
    out = +"\x04\x08"
    _dump(obj, out, st)
    out.force_encoding("ASCII-8BIT") if out.respond_to?(:force_encoding)
    if an_io
      an_io.write(out); an_io
    else
      out
    end
  end

  # --- dump ------------------------------------------------------------------

  # CRuby marshals the real class name; a user-defined `self.name` override is
  # ignored, so go through Module#name directly.
  MODULE_NAME = Module.instance_method(:name)
  def self._class_name(k) = MODULE_NAME.bind_call(k)

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
    # A negative limit means unlimited; otherwise each nesting level costs one.
    lim = st[:limit]
    if lim && lim >= 0
      raise ArgumentError, "exceed depth limit" if st[:depth] > lim
      st[:depth] += 1
      begin
        _dump_val(o, out, st)
      ensure
        st[:depth] -= 1
      end
    else
      _dump_val(o, out, st)
    end
  end

  def self._dump_val(o, out, st)
    case o
    when Class  then return _dump_class(o, out, st)    # (before String: Class subjects mis-match `when String`)
    when Module
      n = _class_name(o)
      raise TypeError, "can't dump anonymous module #{o}" if n.nil?
      _dump_named(o, "m", n, out, st)
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
    when Struct then _dump_struct(o, out, st)
    when Rational                                      # 'U' user-marshal
      out << "U"; _symdump(:Rational, out, st); _dump([o.numerator, o.denominator], out, st)
    when Complex
      out << "U"; _symdump(:Complex, out, st); _dump([o.real, o.imaginary], out, st)
    else
      _raise_if_undumpable(o)
      if defined?(Data) && Data === o
        _dump_data(o, out, st)
      elsif o.respond_to?(:marshal_dump)
        _dump_umarshal(o, out, st)
      elsif o.respond_to?(:_dump)
        _dump_udump(o, out, st)
      elsif Exception === o
        _dump_exception(o, out, st)
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
      nm = _class_name(o.class)
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
    if o.compare_by_identity?                           # CRuby marks this with a 'C:Hash' wrapper
      out << "C"; _symdump(:Hash, out, st)
    end
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

  # CRuby raises TypeError for objects with no _dump_data (unmarshalable core types).
  UNDUMPABLE = %w[MatchData IO File Thread Mutex Thread::Mutex Binding
                  Method UnboundMethod].freeze
  def self._raise_if_undumpable(o)
    if Proc === o || Method === o
      raise TypeError, "no _dump_data is defined for class #{o.class}"
    end
    UNDUMPABLE.each do |n|
      k = (Object.const_get(n) rescue nil)
      if k && o.is_a?(k)
        raise TypeError, "no _dump_data is defined for class #{o.class}"
      end
    end
  end

  def self._dump_struct(o, out, st)
    name = _class_name(o.class)
    raise TypeError, "can't dump anonymous class #{o.class}" if name.nil?
    uiv = o.instance_variables
    out << "I" unless uiv.empty?
    _wrap_prefix(o, o.class, out, st)                  # 'e' extends only
    out << "S"; _symdump(name.to_sym, out, st)
    mem = o.members; vals = o.to_a
    _long(mem.length, out)
    mem.each_with_index { |m, i| _symdump(m, out, st); _dump(vals[i], out, st) }
    _dump_ivars(o, uiv, out, st) unless uiv.empty?
  end

  def self._dump_data(o, out, st)
    name = _class_name(o.class)
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
    n = _class_name(o)
    raise TypeError, "can't dump anonymous class #{o}" if n.nil?
    raise TypeError, "singleton can't be dumped" if o.inspect.start_with?("#<Class:")
    _dump_named(o, "c", n, out, st)
  end

  # 'c'/'m' record whose payload is the class/module NAME.  A non-ASCII name
  # carries the name's encoding, like any other String (CRuby wraps it in 'I').
  def self._dump_named(o, tag, n, out, st)
    if n.ascii_only?
      out << tag; _long(n.bytesize, out); out << n
    else
      out << "I" << tag; _long(n.bytesize, out); out << n
      _long(1, out); _write_enc(_str_enc_marker(n), out, st)
    end
  end

  def self._dump_umarshal(o, out, st)
    name = _class_name(o.class)
    raise TypeError, "can't dump anonymous class #{o.class}" if name.nil?
    d = o.marshal_dump
    out << "U"; _symdump(name.to_sym, out, st); _dump(d, out, st)
  end

  # A Time built with a timezone OBJECT carries that object as #zone; CRuby
  # marshals its #name (and lets the NoMethodError through when it has none).
  def self._time_zone_name(z)
    return z.dup.force_encoding("US-ASCII") if z.is_a?(String)
    return nil if z.nil?
    z.name.to_s.dup.force_encoding("US-ASCII")
  end

  def self._dump_udump(o, out, st)
    name = _class_name(o.class)
    raise TypeError, "can't dump anonymous class #{o.class}" if name.nil?
    d = o._dump(-1)
    unless String === d
      raise TypeError, "_dump() must return string, not #{d.class}"
    end
    enc = _str_enc_marker(d)
    ivars = d.instance_variables
    # Time's zone rides along as the pseudo-ivars :offset / :zone (no '@' — CRuby
    # writes them straight into the payload's ivar table, so they are invisible
    # to #instance_variables).
    extra = if Time === o
              o.utc? ? [[:zone, "UTC".dup.force_encoding("US-ASCII")]]   # UTC: the zone alone, no offset
                     : [[:offset, o.utc_offset], [:zone, _time_zone_name(o.zone)]]
            else []
            end
    if Time === o && (sub = o.nsec % 1000) != 0
      # digits below the microsecond, packed BCD without sign (CRuby's :submicro):
      # 789ns → "\x78\x90", 700ns → "\x70"
      b0 = ((sub / 100) << 4) | ((sub / 10) % 10)
      b1 = (sub % 10) << 4
      extra.unshift([:submicro, (b1 == 0 ? [b0] : [b0, b1]).pack("C*")])
    end
    if enc || !ivars.empty? || !extra.empty?
      out << "I"
      out << "u"; _symdump(name.to_sym, out, st); _long(d.bytesize, out); out << d
      _long((enc ? 1 : 0) + ivars.length + extra.length, out)
      _write_enc(enc, out, st) if enc
      extra.each { |nm, v| _symdump(nm, out, st); _dump(v, out, st) }
      ivars.each { |iv| _symdump(iv, out, st); _dump(d.instance_variable_get(iv), out, st) }
    else
      out << "u"; _symdump(name.to_sym, out, st); _long(d.bytesize, out); out << d
    end
  end

  # Exception: CRuby stores the message and backtrace as the pseudo-ivars
  # :mesg and :bt (no '@'), ahead of any real instance variables.
  def self._dump_exception(o, out, st)
    name = _class_name(o.class)
    raise TypeError, "can't dump anonymous class #{o.class}" if name.nil?
    _wrap_prefix(o, o.class, out, st)
    out << "o"; _symdump(name.to_sym, out, st)
    uiv = o.instance_variables
    _long(2 + uiv.length, out)
    _symdump(:mesg, out, st); _dump(o.message, out, st)
    _symdump(:bt, out, st);   _dump(o.backtrace, out, st)
    uiv.each { |iv| _symdump(iv, out, st); _dump(o.instance_variable_get(iv), out, st) }
  end

  def self._dump_generic(o, out, st)
    name = _class_name(o.class)
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

  def self.load(data, proc = nil, freeze: false)
    from_io = false
    if data.respond_to?(:read) && !data.is_a?(String)
      from_io = true; data = data.read
    end
    data = data.string if data.respond_to?(:string)
    if data.nil? || data.bytesize == 0
      raise from_io ? EOFError.new("end of file reached") : ArgumentError.new("marshal data too short")
    end
    st = { s: data, i: 0, syms: [], objs: [], proc: proc, freeze: freeze }
    maj = _byte(st); min = _byte(st)
    if maj != MAJOR_VERSION || min > MINOR_VERSION
      raise TypeError,
            "incompatible marshal file format (can't be read)\n" \
            "\tformat version #{MAJOR_VERSION}.#{MINOR_VERSION} required; " \
            "#{maj}.#{min} given"
    end
    _read(st)
  end
  class << self; alias restore load; end

  # Resolve a possibly '::'-qualified constant name for load; ArgumentError if absent.
  def self._const(name)
    name.to_s.split("::").reduce(Object) { |m, n| m.const_get(n) }
  rescue NameError
    raise ArgumentError, "undefined class/module #{name}"
  end

  def self._byte(st)
    b = st[:s].getbyte(st[:i])
    raise ArgumentError, "marshal data too short" if b.nil?
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
      if cls == Hash                                     # 'C:Hash' marks compare_by_identity
        inner.compare_by_identity
      else
        h = cls.new; inner.each { |k, v| h[k] = v }; h
      end
    when Regexp
      cls.new(inner.source, inner.options)
    else inner
    end
  rescue StandardError
    inner
  end

  # Object#freeze itself, so a user-defined #freeze is not called (CRuby freezes
  # internally).
  FREEZE = Kernel.instance_method(:freeze)

  def self._read(st)
    v = _read0(st)
    was_link = st[:link]; st[:link] = false   # consume: a nested read has already cleared its own
    # `freeze: true` hands out frozen objects — freeze on the way out, once the
    # container has been filled, and before the caller's proc sees it.  Classes
    # and modules are left alone (CRuby does not freeze them).
    FREEZE.bind_call(v) if st[:freeze] && !v.is_a?(Module)
    # The proc sees each object as it is BUILT (a back-reference to one already
    # built does not fire it again) and its return value replaces the object.
    p = st[:proc]
    (p && !was_link) ? p.call(v) : v
  end

  def self._read0(st)
    t = _byte(st)
    case t
    when 0x30 then nil                                  # '0'
    when 0x54 then true                                 # 'T'
    when 0x46 then false                                # 'F'
    when 0x69 then _rlong(st)                            # 'i' Fixnum
    when 0x40 then st[:link] = true; st[:objs][_rlong(st)]   # '@' object link (no proc callback)
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
      # Unwrapped strings are ASCII-8BIT; an enclosing 'I' with :E / :encoding
      # re-tags them.
      _reg(st, _bytes(st, _rlong(st)).force_encoding("ASCII-8BIT"))
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
      if st[:s].getbyte(st[:i]) == 0x75                  # 'u' user _dump
        # CRuby attaches these ivars to the _dump payload String and only then
        # calls ::_load — that is how Time picks up its @offset / @zone.
        _byte(st)
        idx = st[:objs].size; st[:objs] << nil
        cls = _read0(st)
        data = _bytes(st, _rlong(st))
        offset = nil; submicro = nil; zname = nil
        _rlong(st).times do
          name = _read0(st); val = _read(st)
          next if name == :E || name == :encoding
          offset = val if name == :offset
          submicro = val if name == :submicro
          zname = val if name == :zone
          data.instance_variable_set(name, val) rescue nil   # :offset / :zone have no '@'
        end
        obj = _const(cls)._load(data)
        if Time === obj && submicro                        # restore the sub-microsecond digits
          b = submicro.bytes
          sub = ((b[0] >> 4) * 100) + ((b[0] & 0xF) * 10) + (b[1] ? (b[1] >> 4) : 0)
          if sub != 0
            t2 = Time.at(obj.to_i, Rational(obj.nsec + sub, 1000))   # exact: no Float round-trip
            obj = obj.utc? ? t2.utc : t2
          end
        end
        # A non-UTC Time was packed in local wall-clock fields: shift back to the
        # instant, then re-attach the fixed offset so #utc_offset survives.  The
        # shift is done on the integer seconds so the nanoseconds stay exact
        # (Time#- goes through a Float).
        obj = Time.at(obj.to_i - offset, Rational(obj.nsec, 1000)).getlocal(offset) if Time === obj && offset
        # A zone name rides along as the pseudo-ivar :zone.  CRuby rebuilds the
        # timezone OBJECT from it through the class's .find_timezone hook; with no
        # hook the name itself becomes #zone.
        if Time === obj && String === zname && !obj.utc?
          k = _const(cls)
          tz = (k.respond_to?(:find_timezone) ? (k.find_timezone(zname) rescue nil) : nil)
          if tz
            obj = Time.at(obj.to_i, Rational(obj.nsec, 1000)).getlocal(tz)
          else
            obj.instance_variable_set(:@__tz, zname)
          end
        end
        st[:objs][idx] = obj
        return obj
      end
      v = _read0(st)                                     # transparent: caller's proc sees the object
      ni = _rlong(st)
      ni.times do
        name = _read0(st); val = _read(st)              # ivar name is structural; value is data
        if name == :E                                   # encoding marker, not a user ivar
          v.force_encoding(val ? "UTF-8" : "US-ASCII") if v.respond_to?(:force_encoding)
          next
        end
        if name == :encoding
          (v.force_encoding(val) rescue nil) if v.respond_to?(:force_encoding)
          next
        end
        v.instance_variable_set(name, val) rescue nil
      end
      v
    when 0x2f                                            # '/' Regexp
      src = _bytes(st, _rlong(st)); opt = _byte(st)
      _reg(st, Regexp.new(src, opt))
    when 0x43                                            # 'C' subclass wrapper
      idx = st[:objs].size; st[:objs] << nil
      cls = _const(_read0(st))
      st[:reuse] = idx
      inner = _read(st)
      obj = _reclass(cls, inner)
      st[:objs][idx] = obj
    when 0x65                                            # 'e' extend-module wrapper
      msym = _read0(st)
      obj = _read(st)
      (obj.extend(_const(msym)) rescue nil)
      obj
    when 0x63                                            # 'c' Class
      _reg(st, _const(_bytes(st, _rlong(st))))
    when 0x6d                                            # 'm' Module
      _reg(st, _const(_bytes(st, _rlong(st))))
    when 0x6f                                            # 'o' generic object
      idx = st[:objs].size; st[:objs] << nil
      cls = _read0(st)
      ni = _rlong(st)
      ivars = {}
      ni.times { name = _read0(st); ivars[name] = _read(st) }
      if cls == :Range
        obj = Range.new(ivars[:begin], ivars[:end], ivars[:excl])
      else
        klass = _const(cls)
        if klass <= Exception                            # :mesg/:bt are pseudo-ivars
          obj = klass.new(ivars[:mesg])
          obj.set_backtrace(ivars[:bt]) if ivars[:bt] && obj.respond_to?(:set_backtrace)
          ivars.each { |name, val| obj.instance_variable_set(name, val) if name.to_s.start_with?("@") }
        else
          obj = klass.allocate
          ivars.each { |name, val| obj.instance_variable_set(name, val) }
        end
      end
      st[:objs][idx] = obj
    when 0x75                                            # 'u' user _dump
      idx = st[:objs].size; st[:objs] << nil
      cls = _read0(st)
      data = _bytes(st, _rlong(st))
      obj = _const(cls)._load(data)
      st[:objs][idx] = obj
    when 0x53                                            # 'S' Struct
      idx = st[:objs].size; st[:objs] << nil
      cls = _read0(st)
      n = _rlong(st)
      vals = []
      n.times { _read0(st); vals << _read(st) }
      st[:objs][idx] = _const(cls).new(*vals)
    when 0x55                                            # 'U' user marshal_dump
      idx = st[:objs].size; st[:objs] << nil
      cls = _read0(st)
      data = _read(st)
      obj = case cls
            when :Rational then Rational(data[0], data[1])
            when :Complex  then Complex(data[0], data[1])
            else
              o = _const(cls).allocate
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
    # a truncated stream is a data error, not a nil payload
    raise ArgumentError, "marshal data too short" if r.nil? || r.bytesize < n
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
